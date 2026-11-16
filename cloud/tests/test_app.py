import base64,hashlib,importlib.util,json,os,sys,types,unittest
from pathlib import Path
class Table:
 def __init__(self):self.data={}
 def get_item(self,**kw):return {'Item':dict(self.data)}
 def update_item(self,**kw):
  names=kw.get('ExpressionAttributeNames',{});v=kw.get('ExpressionAttributeValues',{});e=kw['UpdateExpression']
  if e=='SET #s=:s, settings=:c, seen=:t':self.data.update(status=v[':s'],settings=v[':c'],seen=v[':t'])
  elif e=='SET #slot=:v':self.data[names['#slot']]=v[':v']
  elif e=='REMOVE #k':
   if self.data.get(names['#k'])==v[':old']:self.data.pop(names['#k'],None)
class ClientError(Exception):pass
table=Table();objects={}
class S3:
 def put_object(self,**kw):objects[kw['Key']]=kw['Body']
 def get_object(self,**kw):return {'Body':types.SimpleNamespace(read=lambda:objects[kw['Key']])}
sys.modules['boto3']=types.SimpleNamespace(resource=lambda x:types.SimpleNamespace(Table=lambda n:table),client=lambda x:S3())
sys.modules['botocore']=types.ModuleType('botocore');sys.modules['botocore.exceptions']=types.SimpleNamespace(ClientError=ClientError)
os.environ['ADMIN_TOKEN_HASH']=hashlib.sha256(b'admin').hexdigest();os.environ['DEVICE_TOKEN_HASH']=hashlib.sha256(b'device').hexdigest()
spec=importlib.util.spec_from_file_location('app',Path(__file__).parents[1]/'app.py');app=importlib.util.module_from_spec(spec);spec.loader.exec_module(app)
def request(path,method='GET',body=None,token='admin'):
 return app.handler({'rawPath':path,'requestContext':{'http':{'method':method}},'headers':{'authorization':'Bearer '+token},'body':json.dumps(body or {})},None)
class Tests(unittest.TestCase):
 def setUp(self):table.data.clear();objects.clear()
 def test_auth(self):
  self.assertEqual(request('/api/status',token='wrong')['statusCode'],401)
  self.assertEqual(request('/device/sync','POST',{'status':{},'settings':{}},'admin')['statusCode'],401)
  self.assertEqual(request('/')['statusCode'],200)
 def test_waiting_and_live(self):
  self.assertTrue(json.loads(request('/api/status')['body'])['waitingForDevice'])
  self.assertEqual(request('/device/sync','POST',{'status':{'state':'SAFE'},'settings':{'home':'Test'}},'device')['statusCode'],200)
  s=json.loads(request('/api/status')['body']);self.assertTrue(s['cloudConnected']);self.assertEqual(s['home']['home'],'Test')
 def test_command_retry_ack(self):
  r=request('/api/readings','POST');cid=json.loads(r['body'])['command']
  body={'status':{},'settings':{}}
  for _ in range(2):self.assertEqual(json.loads(request('/device/sync','POST',body,'device')['body'])['commands'][0]['id'],cid)
  body['acks']=[cid];self.assertFalse(json.loads(request('/device/sync','POST',body,'device')['body'])['commands'])
 def test_capture_queue_and_ack(self):
  self.assertEqual(request('/api/capture','POST',token='wrong')['statusCode'],401)
  result=request('/api/capture','POST');self.assertEqual(result['statusCode'],202)
  cid=json.loads(result['body'])['command'];cmd=json.loads(table.data['cmd_capture'])
  self.assertEqual(cmd['kind'],'capture');self.assertLessEqual(cmd['expires']-int(app.time.time()),30)
  body={'status':{},'settings':{}}
  cmds=json.loads(request('/device/sync','POST',body,'device')['body'])['commands']
  self.assertEqual(cmds[0]['id'],cid)
  body['acks']=[cid]
  self.assertFalse(json.loads(request('/device/sync','POST',body,'device')['body'])['commands'])
  self.assertNotIn('sms_test_at',table.data)
 def test_waiting_has_sms_configuration(self):
  body=json.loads(request('/api/status')['body']);self.assertIn('cloudSms',body)
  self.assertFalse(body['cloudSms']['enabled'])
 def test_hybrid_location(self):
  loc={'valid':True,'source':'wifi','latitude':18.9558,'longitude':72.8119,'accuracy':35,'mismatch':False}
  body={'status':{'state':'SAFE','softwareRevision':'hybrid-location-v3','location':loc,'stationSms':False},'settings':{'home':'Test'}}
  self.assertEqual(request('/device/sync','POST',body,'device')['statusCode'],200)
  status=json.loads(request('/api/status')['body']);self.assertEqual(status['location'],loc)
  self.assertEqual(status['softwareRevision'],'hybrid-location-v3')
 def test_location_settings_queue(self):
  r=request('/api/settings','POST',{'hasLocation':True,'latitude':18.9558,'longitude':72.8119})
  self.assertEqual(r['statusCode'],202)
  for field in ['geoApiKey','geoEnabled','stationSms','smsApiKey','token']:
   self.assertEqual(request('/api/settings','POST',{field:'secret'})['statusCode'],400)
 def test_local_secrets(self):self.assertEqual(request('/api/settings','POST',{'token':'secret'})['statusCode'],400)
 def test_private_photo(self):
  b=b'\xff\xd8\xff\xd9';request('/device/sync','POST',{'status':{},'settings':{},'photo':base64.b64encode(b).decode()},'device')
  self.assertEqual(request('/api/photo',token='wrong')['statusCode'],401)
  self.assertEqual(base64.b64decode(request('/api/photo')['body']),b)
if __name__=='__main__':unittest.main()
