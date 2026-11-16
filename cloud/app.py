"""FireGuard single-home HTTPS application; Lambda Function URL + private S3 + DynamoDB."""
import base64,hashlib,hmac,json,mimetypes,os,re,time,uuid
from pathlib import Path
import boto3
from botocore.exceptions import ClientError
TABLE=os.environ.get('TABLE_NAME','fireguard')
BUCKET=os.environ.get('BUCKET_NAME','fireguard')
DEVICE_ID='fireguard-01'
ddb=boto3.resource('dynamodb').Table(TABLE)
s3=boto3.client('s3')

# ---- Optional SMS through Amazon SNS -------------------------------------------------------------
# Cloud-side and independent of the ESP32's textbee SMS. Off unless the stack sets SMS_ENABLED=true.
# Without DLT registration, SNS sends to India over international routes (random numeric sender).
SMS_ENABLED=os.environ.get('SMS_ENABLED','false').lower()=='true'
sns=boto3.client('sns') if SMS_ENABLED else None
SMS_STATES={'FIRE DETECTED':'FIRE','CRITICAL':'FIRE','GAS LEAK DETECTED':'GAS'}
SMS_TEXT={'FIRE':'FIRE ALERT: fire detected. Leave now; seek emergency help.',
          'GAS':'GAS ALERT: gas/smoke activity. Check now; avoid flames and switches.',
          'ALL_CLEAR':'All clear: readings back to SAFE. Check before going back in.',
          'TEST':'TEST ONLY: AWS SMS alerts are working. No action needed.'}
SMS_MAX_PER_INCIDENT=4;SMS_MIN_GAP=60;SMS_TEST_GAP=30

def sms_phone(raw):
    """'+<digits>' or ''. Bare 10-digit Indian mobiles (6-9...), 0+10 or 91+10 digits get +91."""
    if not isinstance(raw,str):return ''
    s=re.sub(r'[\s\-.()]','',raw)
    if s.startswith('+'):
        d=s[1:]
        return '+'+d if d.isdigit() and 8<=len(d)<=15 and d[0]!='0' else ''
    if not s.isdigit():return ''
    if len(s)==11 and s[0]=='0':s=s[1:]
    if len(s)==12 and s.startswith('91'):s=s[2:]
    return '+91'+s if len(s)==10 and s[0] in '6789' else ''

def sms_recipients(settings):
    out=[]
    for c in (settings or {}).get('contacts') or []:
        n=sms_phone((c or {}).get('phone',''))
        if n and n not in out:out.append(n)
    return out

def sms_text(kind,home,event=0):
    home=''.join(ch if 32<=ord(ch)<127 and ch not in '[]{}~^|\\`' else '?' for ch in (home or 'your home'))[:28]
    text='FireGuard ('+home+'): '+SMS_TEXT[kind]
    if event and kind not in ('TEST',):text+=' Ev '+str(event)+'.'
    return text[:160]

def sms_publish(numbers,text):
    attrs={'AWS.SNS.SMS.SMSType':{'DataType':'String','StringValue':'Transactional'}}
    # Only needed after DLT registration (local Indian routes); harmless when unset.
    for env,name in [('SMS_ENTITY_ID','AWS.MM.SMS.EntityId'),('SMS_TEMPLATE_ID','AWS.MM.SMS.TemplateId')]:
        if os.environ.get(env):attrs[name]={'DataType':'String','StringValue':os.environ[env]}
    sent=0;errors=[]
    for n in numbers:
        try:sns.publish(PhoneNumber=n,Message=text,MessageAttributes=attrs);sent+=1
        except ClientError as e:  # numbers are masked (+91******3210) so logs/dashboard don't expose them
            errors.append(n[:3]+'*'*(len(n)-7)+n[-4:]+': '+e.response.get('Error',{}).get('Code','Error'))
    return {'sent':sent,'failed':len(errors),'errors':errors[:4]}

def sms_claim(old_raw,new):
    """Conditional write so two overlapping syncs never send the same SMS twice."""
    try:
        if old_raw is None:
            ddb.update_item(Key={'id':DEVICE_ID},UpdateExpression='SET sms=:new',ConditionExpression='attribute_not_exists(sms)',ExpressionAttributeValues={':new':json.dumps(new)})
        else:
            ddb.update_item(Key={'id':DEVICE_ID},UpdateExpression='SET sms=:new',ConditionExpression='sms=:old',ExpressionAttributeValues={':new':json.dumps(new),':old':old_raw})
        return True
    except ClientError as e:
        if e.response['Error']['Code']=='ConditionalCheckFailedException':return False
        raise

def sms_record(kind,result):
    result=dict(result,kind=kind,at=int(time.time()))
    ddb.update_item(Key={'id':DEVICE_ID},UpdateExpression='SET sms_result=:r',ExpressionAttributeValues={':r':json.dumps(result)})
    print('FireGuard SMS',kind,'sent',result['sent'],'failed',result['failed'])

def sms_on_sync(before,status,settings):
    """Called after each device sync: SMS on entering FIRE/GAS/CO, all-clear on return to SAFE."""
    if not sns:return
    state=status.get('state');kind=SMS_STATES.get(state);old_raw=before.get('sms')
    rec=json.loads(old_raw) if old_raw else {'active':False}
    now=int(time.time());send=None;new=dict(rec)
    if kind:
        if not rec.get('active'):new={'active':True,'kinds':[],'batches':0,'last':0}
        if kind not in new['kinds'] and new['batches']<SMS_MAX_PER_INCIDENT-1 and now-new['last']>=SMS_MIN_GAP:
            send=kind;new['kinds']=new['kinds']+[kind];new['batches']+=1;new['last']=now
    elif state=='SAFE' and rec.get('active'):
        send='ALL_CLEAR' if rec.get('notified') else None;new={'active':False}
        if not send:
            sms_claim(old_raw,new)
            return
    if not send or not sms_claim(old_raw,new):return
    numbers=sms_recipients(settings)
    if not numbers:return sms_record(send,{'sent':0,'failed':0,'errors':['No resident has a valid phone number']})
    event=status.get('event') if isinstance(status.get('event'),int) else 0
    result=sms_publish(numbers,sms_text(send,settings.get('home'),event))
    sms_record(send,result)
    if kind and result['sent']:
        sms_claim(json.dumps(new),dict(new,notified=True))

def sms_test(state):
    if not sns:return response(400,{'error':'AWS SMS is off for this deployment. Redeploy with FIREGUARD_SMS_ENABLED=true.'})
    if time.time()-int(state.get('sms_test_at',0))<SMS_TEST_GAP:return response(429,{'error':'Wait 30 seconds between AWS test SMS'})
    settings=json.loads(state.get('settings','{}'));numbers=sms_recipients(settings)
    if not numbers:return response(400,{'error':'No resident has a valid phone number yet (the ESP32 must sync its settings first)'})
    try:
        ddb.update_item(Key={'id':DEVICE_ID},UpdateExpression='SET sms_test_at=:t',ConditionExpression='attribute_not_exists(sms_test_at) OR sms_test_at <= :cutoff',ExpressionAttributeValues={':t':int(time.time()),':cutoff':int(time.time())-SMS_TEST_GAP})
    except ClientError as e:
        if e.response['Error']['Code']=='ConditionalCheckFailedException':return response(429,{'error':'Wait 30 seconds between AWS test SMS'})
        raise
    result=sms_publish(numbers,sms_text('TEST',settings.get('home')));sms_record('TEST',result)
    msg='AWS test SMS accepted for '+str(result['sent'])+' number(s)'
    if result['failed']:msg+='; failed: '+', '.join(result['errors'])+' (in the SNS sandbox, verify each number first)'
    return response(200,{'message':msg,**result})
# --------------------------------------------------------------------------------------------------

def response(code,body,kind='application/json'):
    if kind=='application/json':body=json.dumps(body,separators=(',',':'),allow_nan=False)
    return {'statusCode':code,'headers':{'content-type':kind,'cache-control':'no-store','x-content-type-options':'nosniff','referrer-policy':'no-referrer','content-security-policy':"default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; frame-ancestors 'none'"},'body':body,'isBase64Encoded':False}

def authorized(headers,role):
    raw=headers.get('authorization','')
    expected=os.environ.get(role+'_TOKEN_HASH','')
    return bool(expected) and raw.startswith('Bearer ') and hmac.compare_digest(hashlib.sha256(raw[7:].encode()).hexdigest(),expected)

def item():return ddb.get_item(Key={'id':DEVICE_ID},ConsistentRead=True).get('Item',{})

def command(kind,data=None):
    # One slot per command type bounds memory while retaining commands until acknowledged.
    cid=uuid.uuid4().hex
    value=json.dumps({'id':cid,'kind':kind,'data':data or {},'expires':int(time.time())+(30 if kind=='capture' else 300)})
    ddb.update_item(Key={'id':DEVICE_ID},UpdateExpression='SET #slot=:v',ExpressionAttributeNames={'#slot':'cmd_'+kind},ExpressionAttributeValues={':v':value})
    return response(202,{'message':'Queued for the ESP32. It will apply this when connected.','command':cid})

def upload(data):
    status=data.get('status');settings=data.get('settings');acks=data.get('acks',[])
    if not isinstance(status,dict) or not isinstance(settings,dict) or len(json.dumps(status))>16000 or len(json.dumps(settings))>20000:
        return response(400,{'error':'Invalid device snapshot'})
    photo=data.get('photo')
    if photo:
        if not isinstance(photo,str) or len(photo)>400000:return response(400,{'error':'Image too large'})
        try:raw=base64.b64decode(photo,validate=True)
        except ValueError:return response(400,{'error':'Invalid JPEG'})
        if len(raw)>300000 or not raw.startswith(b'\xff\xd8'):return response(400,{'error':'Invalid JPEG'})
        s3.put_object(Bucket=BUCKET,Key=DEVICE_ID+'/latest.jpg',Body=raw,ContentType='image/jpeg',ServerSideEncryption='AES256')
    before=item()
    ddb.update_item(Key={'id':DEVICE_ID},UpdateExpression='SET #s=:s, settings=:c, seen=:t',ExpressionAttributeNames={'#s':'status'},ExpressionAttributeValues={':s':json.dumps(status,allow_nan=False),':c':json.dumps(settings,allow_nan=False),':t':int(time.time())})
    for slot in ['ack','test','readings','settings','emergency_test','capture']:
        key='cmd_'+slot
        raw=before.get(key)
        if not raw:continue
        cmd=json.loads(raw)
        if cmd['id'] in acks or cmd['expires']<time.time():
            try:ddb.update_item(Key={'id':DEVICE_ID},UpdateExpression='REMOVE #k',ConditionExpression='#k=:old',ExpressionAttributeNames={'#k':key},ExpressionAttributeValues={':old':raw})
            except ClientError as e:
                if e.response['Error']['Code']!='ConditionalCheckFailedException':raise
    try:sms_on_sync(before,status,settings)
    except Exception:print('FireGuard SMS step failed')  # never block the device sync
    current=item();commands=[]
    for slot in ['ack','test','readings','settings','emergency_test','capture']:
        if current.get('cmd_'+slot):
            cmd=json.loads(current['cmd_'+slot])
            if cmd['expires']>=time.time():commands.append(cmd)
    return response(200,{'commands':commands})

def handler(event,context):
    try:
        path=event.get('rawPath','/')
        method=event.get('requestContext',{}).get('http',{}).get('method','GET')
        headers={k.lower():v for k,v in event.get('headers',{}).items()}
        if method=='GET' and path in ['/','/index.html']:
            return response(200,Path(__file__).with_name('web').joinpath('index.html').read_text(),'text/html; charset=utf-8')
        if method=='GET' and path=='/health':return response(200,{'service':'FireGuard','revision':'hybrid-location-v3','hardwareRevision':'two-mq-ads1115-v2'})
        role='DEVICE' if path=='/device/sync' else 'ADMIN'
        if not authorized(headers,role):return response(401,{'error':'Sign in to FireGuard'})
        raw=event.get('body') or '{}'
        if event.get('isBase64Encoded'):raw=base64.b64decode(raw).decode()
        if len(raw)>450000:return response(413,{'error':'Request too large'})
        data=json.loads(raw)
        if not isinstance(data,dict):return response(400,{'error':'Expected JSON object'})
        if path=='/device/sync' and method=='POST':return upload(data)
        state=item();seen=int(state.get('seen',0));online=time.time()-seen<25
        if path=='/api/status' and method=='GET':
            if not state.get('status'):return response(200,{'waitingForDevice':True,'cloudConnected':False,'lastSeen':0,'csrf':'cloud','cloudSms':{'enabled':SMS_ENABLED,'last':None}})
            status=json.loads(state['status']);status.update({'setup':True,'csrf':'cloud','cloudConnected':online,'lastSeen':seen,'home':json.loads(state.get('settings','{}'))})
            status['cloudSms']={'enabled':SMS_ENABLED,'last':json.loads(state['sms_result']) if state.get('sms_result') else None}
            return response(200,status)
        if path=='/api/settings' and method=='GET':
            return response(200,json.loads(state.get('settings','{}')))
        if path=='/api/settings' and method=='POST':
            allowed={'home','addressFields','landmark','station','stationPhone','ambulancePhone','emergencyPhone','pin','emergencyPin','emergencyEnabled','emergencyTest','unattendedSeconds','fireConfidence','contacts','removeEmergency','latitude','longitude','hasLocation'}
            if any(k not in allowed for k in data):return response(400,{'error':'Wi-Fi, Telegram and device credentials are configured locally on the ESP32'})
            return command('settings',data)
        if path=='/api/ack' and method=='POST':
            if not isinstance(data.get('event'),int) or data['event']<1:return response(400,{'error':'Invalid event'})
            return command('ack',{'event':data['event']})
        if path in ['/api/test','/api/readings','/api/emergency-test','/api/capture'] and method=='POST':
            return command({'/api/test':'test','/api/readings':'readings','/api/emergency-test':'emergency_test','/api/capture':'capture'}[path])
        if path=='/api/sms-test' and method=='POST':return sms_test(state)
        if path=='/api/photo' and method=='GET':
            try:r=s3.get_object(Bucket=BUCKET,Key=DEVICE_ID+'/latest.jpg')
            except ClientError as e:
                if e.response['Error']['Code'] in ['NoSuchKey','404']:return response(404,{'error':'No scored camera image uploaded yet'})
                raise
            rbody=r['Body'].read()
            return {'statusCode':200,'headers':{'content-type':'image/jpeg','cache-control':'no-store','x-content-type-options':'nosniff'},'body':base64.b64encode(rbody).decode(),'isBase64Encoded':True}
        return response(404,{'error':'Not found'})
    except (ValueError,TypeError,KeyError):return response(400,{'error':'Invalid request'})
    except Exception:
        # Do not log credentials, images, resident addresses or request bodies.
        print('FireGuard request failed')
        return response(500,{'error':'Server unavailable; local sensing remains on the ESP32'})
