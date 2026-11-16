"""AWS SNS SMS tests with local fakes (no AWS calls): python cloud/tests/test_sms.py"""
import hashlib,importlib.util,json,os,sys,types,unittest
from pathlib import Path

class ClientError(Exception):
    def __init__(self,code):super().__init__(code);self.response={'Error':{'Code':code}}

class Table:
    def __init__(self):self.data={}
    def get_item(self,**kw):return {'Item':dict(self.data)}
    def update_item(self,**kw):
        names=kw.get('ExpressionAttributeNames',{});v=kw.get('ExpressionAttributeValues',{});e=kw['UpdateExpression']
        cond=kw.get('ConditionExpression')
        if cond=='attribute_not_exists(sms)' and 'sms' in self.data:raise ClientError('ConditionalCheckFailedException')
        if cond=='sms=:old' and self.data.get('sms')!=v[':old']:raise ClientError('ConditionalCheckFailedException')
        if e=='SET #s=:s, settings=:c, seen=:t':self.data.update(status=v[':s'],settings=v[':c'],seen=v[':t'])
        elif e=='SET #slot=:v':self.data[names['#slot']]=v[':v']
        elif e=='SET sms=:new':self.data['sms']=v[':new']
        elif e=='SET sms_result=:r':self.data['sms_result']=v[':r']
        elif e=='SET sms_test_at=:t':self.data['sms_test_at']=v[':t']
        elif e=='REMOVE #k':
            if self.data.get(names['#k'])==v[':old']:self.data.pop(names['#k'],None)
        else:raise AssertionError('unexpected update '+e)

class SNS:
    def __init__(self):self.sent=[];self.reject=set()
    def publish(self,**kw):
        if kw['PhoneNumber'] in self.reject:raise ClientError('AuthorizationError')
        self.sent.append(kw)

table=Table();sns=SNS()
class S3:
    def put_object(self,**kw):pass
sys.modules['boto3']=types.SimpleNamespace(resource=lambda x:types.SimpleNamespace(Table=lambda n:table),
                                           client=lambda x:sns if x=='sns' else S3())
sys.modules['botocore']=types.ModuleType('botocore');sys.modules['botocore.exceptions']=types.SimpleNamespace(ClientError=ClientError)
os.environ.update(ADMIN_TOKEN_HASH=hashlib.sha256(b'admin').hexdigest(),DEVICE_TOKEN_HASH=hashlib.sha256(b'device').hexdigest(),SMS_ENABLED='true')
spec=importlib.util.spec_from_file_location('app_sms',Path(__file__).parents[1]/'app.py');app=importlib.util.module_from_spec(spec);spec.loader.exec_module(app)

SETTINGS={'home':'Test Kitchen','contacts':[{'name':'A','phone':'98765 43210'},{'name':'B','phone':'12345'},
          {'name':'C','phone':'+44 7700 900123'},{'name':'D','phone':'+919876543210'},{'name':'E','phone':''}]}
def call(path,method='GET',body=None,token='admin'):
    return app.handler({'rawPath':path,'requestContext':{'http':{'method':method}},'headers':{'authorization':'Bearer '+token},'body':json.dumps(body or {})},None)
def sync(state,event=1):
    r=call('/device/sync','POST',{'status':{'state':state,'event':event},'settings':SETTINGS},'device')
    assert r['statusCode']==200,r
def age(seconds):  # pretend the last SMS was sent `seconds` earlier
    rec=json.loads(table.data['sms']);rec['last']-=seconds;table.data['sms']=json.dumps(rec)

class Tests(unittest.TestCase):
    def setUp(self):table.data.clear();sns.sent.clear();sns.reject.clear()

    def test_phone_numbers(self):
        p=app.sms_phone
        self.assertEqual(p('9876543210'),'+919876543210');self.assertEqual(p('098765-43210'),'+919876543210')
        self.assertEqual(p('919876543210'),'+919876543210');self.assertEqual(p('+1 (202) 555-0123'),'+12025550123')
        for bad in ['12345','5876543210','98765x43210','+0123456789','',None]:self.assertEqual(p(bad),'')
        self.assertEqual(app.sms_recipients(SETTINGS),['+919876543210','+447700900123'])

    def test_text(self):
        t=app.sms_text('FIRE','H'*80+'\U0001f525',4294967295)
        self.assertLessEqual(len(t),160);self.assertTrue(all(32<=ord(c)<127 for c in t))
        self.assertIn('FIRE ALERT',app.sms_text('FIRE','Home',7));self.assertIn('Ev 7.',app.sms_text('FIRE','Home',7))

    def test_incident_flow(self):
        sync('SAFE');self.assertEqual(sns.sent,[])                       # nothing while safe
        sync('FIRE DETECTED',5)
        self.assertEqual([m['PhoneNumber'] for m in sns.sent],['+919876543210','+447700900123'])
        m=sns.sent[0];self.assertIn('FIRE ALERT',m['Message']);self.assertIn('Ev 5.',m['Message'])
        self.assertEqual(m['MessageAttributes']['AWS.SNS.SMS.SMSType']['StringValue'],'Transactional')
        self.assertNotIn('AWS.MM.SMS.EntityId',m['MessageAttributes'])
        for _ in range(5):sync('FIRE DETECTED',5)                         # 5-second syncs: no repeats
        sync('CRITICAL',5);self.assertEqual(len(sns.sent),2)               # same kind (FIRE): no repeat
        sync('GAS LEAK DETECTED',5);self.assertEqual(len(sns.sent),2)      # different kind but < 60 s
        age(61);sync('GAS LEAK DETECTED',5);self.assertEqual(len(sns.sent),4)
        self.assertIn('GAS ALERT',sns.sent[-1]['Message'])
        sync('SAFE',5);self.assertEqual(len(sns.sent),6);self.assertIn('All clear',sns.sent[-1]['Message'])
        sync('SAFE',5);self.assertEqual(len(sns.sent),6)                   # one all-clear only
        sync('CO HAZARD',6);self.assertEqual(len(sns.sent),6)              # new incident: immediate
        status=json.loads(call('/api/status')['body'])
        self.assertTrue(status['cloudSms']['enabled']);self.assertEqual(status['cloudSms']['last']['kind'],'ALL_CLEAR')

    def test_warnings_do_not_send(self):
        for s in ['CO HAZARD','WARNING','SMOKE WARNING','FIRE SUSPECTED','GAS LEAK SUSPECT','NORMAL COOKING','SAFE']:sync(s)
        self.assertEqual(sns.sent,[])

    def test_overlapping_syncs_send_once(self):
        before=app.item()  # two Lambda invocations read the same state...
        app.sms_on_sync(before,{'state':'FIRE DETECTED','event':1},SETTINGS)
        app.sms_on_sync(before,{'state':'FIRE DETECTED','event':1},SETTINGS)  # ...second loses the conditional write
        self.assertEqual(len(sns.sent),2)

    def test_sandbox_rejection_is_reported_and_sync_continues(self):
        sns.reject.add('+447700900123');sync('FIRE DETECTED')
        last=json.loads(table.data['sms_result'])
        self.assertEqual((last['sent'],last['failed']),(1,1));self.assertIn('AuthorizationError',last['errors'][0])
        self.assertNotIn('7700900',last['errors'][0])                       # number is masked

    def test_test_endpoint(self):
        self.assertEqual(call('/api/sms-test','POST',token='wrong')['statusCode'],401)
        self.assertEqual(call('/api/sms-test','POST')['statusCode'],400)     # no settings synced yet
        sync('SAFE')
        r=call('/api/sms-test','POST');self.assertEqual(r['statusCode'],200);self.assertIn('TEST ONLY',sns.sent[-1]['Message'])
        self.assertEqual(call('/api/sms-test','POST')['statusCode'],429)     # 30 s cooldown

    def test_dlt_ids_are_attached_when_set(self):
        os.environ.update(SMS_ENTITY_ID='1101',SMS_TEMPLATE_ID='2202')
        try:sync('FIRE DETECTED')
        finally:del os.environ['SMS_ENTITY_ID'];del os.environ['SMS_TEMPLATE_ID']
        a=sns.sent[0]['MessageAttributes'];self.assertEqual(a['AWS.MM.SMS.EntityId']['StringValue'],'1101')
        self.assertEqual(a['AWS.MM.SMS.TemplateId']['StringValue'],'2202')

if __name__=='__main__':unittest.main()
