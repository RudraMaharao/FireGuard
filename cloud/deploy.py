"""Deploy FireGuard to the active AWS account. Secrets stay in a local 0600 provisioning file."""
from pathlib import Path
import hashlib,json,secrets,subprocess,zipfile,os
root=Path(__file__).resolve().parents[1]
work=Path(os.environ['FIREGUARD_DEPLOY_WORK']).resolve();work.mkdir(parents=True,exist_ok=True)
region=os.environ.get('AWS_REGION','ap-south-1');stack='fireguard-live'
def aws(*args):
    p=subprocess.run(['aws','--region',region,*args,'--output','json'],check=True,capture_output=True,text=True)
    return json.loads(p.stdout) if p.stdout.strip() else None
identity=aws('sts','get-caller-identity');account=identity['Account']
subprocess.run([os.sys.executable,str(root/'cloud/build_web.py')],check=True)
secret_file=work/'provisioning.json'
if secret_file.exists():
    keys=json.loads(secret_file.read_text())
    if keys.get('account')!=account or keys.get('region')!=region:
        raise SystemExit('Provisioning account/region mismatch. Use the original deployment work directory.')
    if not all(isinstance(keys.get(k),str) and len(keys[k])>=32 for k in ('adminKey','deviceKey')):
        raise SystemExit('Provisioning keys are missing or invalid; refusing to rotate them.')
else:
    try: existing=aws('cloudformation','describe-stacks','--stack-name',stack)
    except subprocess.CalledProcessError as e:
        if 'does not exist' not in e.stderr:raise
        existing=None
    if existing:
        raise SystemExit('The stack already exists but provisioning.json is absent. Use the original deploy work directory; refusing silent key rotation.')
    keys={'adminKey':secrets.token_urlsafe(32),'deviceKey':secrets.token_urlsafe(32),'region':region,'account':account}
    secret_file.write_text(json.dumps(keys,indent=2));secret_file.chmod(0o600)
artifact_bucket=f'fireguard-code-{account}-{region}'
try:aws('s3api','head-bucket','--bucket',artifact_bucket)
except subprocess.CalledProcessError:
    args=['s3api','create-bucket','--bucket',artifact_bucket]
    if region!='us-east-1':args+=['--create-bucket-configuration',json.dumps({'LocationConstraint':region})]
    aws(*args)
    aws('s3api','put-public-access-block','--bucket',artifact_bucket,'--public-access-block-configuration','BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true')
archive=work/'app.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    z.write(root/'cloud/app.py','app.py');z.write(root/'cloud/web/index.html','web/index.html')
hash=hashlib.sha256(archive.read_bytes()).hexdigest()[:16];key=f'app/{hash}.zip'
aws('s3api','put-object','--bucket',artifact_bucket,'--key',key,'--body',str(archive),'--server-side-encryption','AES256')
params=[{'ParameterKey':'CodeBucket','ParameterValue':artifact_bucket},{'ParameterKey':'CodeKey','ParameterValue':key},*({'ParameterKey':kind+'TokenHash','ParameterValue':hashlib.sha256(keys[name].encode()).hexdigest()} for kind,name in [('Admin','adminKey'),('Device','deviceKey')])]
# Optional AWS SMS: FIREGUARD_SMS_ENABLED=true turns it on (default off). DLT IDs are optional.
params.append({'ParameterKey':'SmsEnabled','ParameterValue':'true' if os.environ.get('FIREGUARD_SMS_ENABLED','false').lower()=='true' else 'false'})
for env,key in [('FIREGUARD_SMS_ENTITY_ID','SmsEntityId'),('FIREGUARD_SMS_TEMPLATE_ID','SmsTemplateId')]:
    if os.environ.get(env):params.append({'ParameterKey':key,'ParameterValue':os.environ[env]})
param_file=work/'parameters.json';param_file.write_text(json.dumps(params));param_file.chmod(0o600)
# aws cloudformation deploy is idempotent and retains a reviewable template.
subprocess.run(['aws','--region',region,'cloudformation','deploy','--stack-name',stack,'--template-file',str(root/'cloud/template.yaml'),'--capabilities','CAPABILITY_IAM','--parameter-overrides',*[p['ParameterKey']+'='+p['ParameterValue'] for p in params],'--no-fail-on-empty-changeset'],check=True)
r=aws('cloudformation','describe-stacks','--stack-name',stack)
outputs={x['OutputKey']:x['OutputValue'] for x in r['Stacks'][0]['Outputs']}
keys.update(outputs);secret_file.write_text(json.dumps(keys,indent=2));secret_file.chmod(0o600)
print(json.dumps(outputs,indent=2))
