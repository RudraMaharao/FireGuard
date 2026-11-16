import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const root=path.resolve(path.dirname(new URL(import.meta.url).pathname),'..');
const local=fs.readFileSync(path.join(root,'firmware/main/web_page.h'),'utf8');
const cloud=fs.readFileSync(path.join(root,'cloud/web/index.html'),'utf8');
function fixture(html,respond){
 const nodes=new Map(),fields=new Map(),calls=[];
 const field=k=>{if(!fields.has(k))fields.set(k,{value:k==='password'?'saved-test-password':k==='token'?'test-token':k==='smsApiKey'?'sms-key':k==='geoApiKey'?'geo-key':k==='pin'?'123456':k==='emergencyPin'?'654321':'test',checked:false});return fields.get(k);};
 const removed=html===cloud?new Set(['ssid','password','token','cloudEnabled','cloudUrl','cloudDeviceToken','smsEnabled','smsApiKey','smsDeviceId','stationSms','geoApiKey','geoEnabled']):new Set();
 const elements=new Proxy({namedItem:k=>removed.has(k)?null:field(k)},{get:(o,k)=>k in o?o[k]:removed.has(k)?undefined:field(k)});
 const node=k=>{if(!nodes.has(k))nodes.set(k,{textContent:k==='save-settings'?'Save home settings':'',disabled:false,elements:k==='form'?elements:undefined,classList:{toggle(){},add(){},remove(){}},style:{},dataset:{}});return nodes.get(k);};
 let script=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]).find(s=>s.includes("let csrf="));
 assert(script);script=script.slice(0,script.lastIndexOf('refresh();'));
 const timers=new Set();
 const context=vm.createContext({document:{getElementById:node,querySelector:()=>null},AbortController,JSON,Number,Error,Math,Date,Promise,
  setTimeout:(fn,ms)=>{const timer=setTimeout(fn,ms===10000?10:ms);timers.add(timer);return timer;},clearTimeout,
  fetch:async(url,options)=>{calls.push({url,options});return respond(url,options,calls.length);}});
 vm.runInContext(script,context);vm.runInContext("loaded=true;csrf='csrf-test';",context);
 return {context,node,field,calls,cleanup:()=>{for(const t of timers)clearTimeout(t);},submit:()=>node('form').onsubmit({preventDefault(){}})};
}
const reply=(status=200,text='ok',pending='0')=>({ok:status>=200&&status<300,status,headers:{get:k=>k==='X-FireGuard-Network-Pending'?pending:null},text:async()=>text});
for(const [html,pending,expected] of [[local,'1',['/api/settings','/api/sms','/api/location','/api/apply-network']],[local,'0',['/api/settings','/api/sms','/api/location']],[cloud,'0',['/api/settings']]]){
 const f=fixture(html,()=>reply(200,'ok',pending));const running=f.submit();assert.equal(f.node('save-settings').disabled,true);assert.match(f.node('save-message').textContent,/Saving/);await running;
 assert.deepEqual(f.calls.map(c=>c.url),expected);assert.equal(f.node('save-settings').disabled,false);assert.match(f.node('save-message').textContent,/All settings saved/);
 assert.equal(f.calls[0].options.headers['X-FireGuard-Defer-Wifi'],'1');
 if(html===local){assert.equal(f.field('token').value,'');assert.equal(f.field('smsApiKey').value,'');}
 else assert(!('ssid' in JSON.parse(f.calls[0].options.body)));
}
{
 const f=fixture(local,()=>reply(400,'Invalid home'));await f.submit();assert.equal(f.calls.length,1);assert.equal(f.field('password').value,'saved-test-password');assert.match(f.node('save-message').textContent,/Invalid home/);assert.equal(f.node('save-settings').disabled,false);
}
{
 const f=fixture(local,url=>url==='/api/sms'?reply(400,'SMS key invalid'):reply(200,'ok','1'));await f.submit();assert.deepEqual(f.calls.map(c=>c.url),['/api/settings','/api/sms','/api/location','/api/apply-network']);assert.match(f.node('save-message').textContent,/Home settings saved.*SMS key invalid/);assert.equal(f.field('smsApiKey').value,'sms-key');assert.equal(f.field('geoApiKey').value,'');assert.equal(f.field('contact_0_name').value,'test');
}
for(const hangingBody of [false,true]){
 const f=fixture(local,()=>hangingBody?{...reply(),text:()=>new Promise(()=>{})}:new Promise(()=>{}));await f.submit();assert.equal(f.calls.length,1);assert.match(f.node('save-message').textContent,/not confirmed.*timed out/);assert.equal(f.field('token').value,'test-token');assert.equal(f.node('save-settings').disabled,false);assert(f.calls[0].options.signal.aborted);
}
{
 const f=fixture(local,url=>{if(url==='/api/apply-network')throw Error('radio lost');return reply(200,'ok','1');});await f.submit();assert.match(f.node('save-message').textContent,/Home settings saved.*within 60 seconds/);
}
{
 let release;const gate=new Promise(r=>release=r);const f=fixture(local,async()=>{await gate;return reply();});const first=f.submit();await f.submit();assert.equal(f.calls.length,1);release();await first;
}
console.log('Local/cloud save order, progress, validation, partial saves, header/body timeouts, reconnect loss and duplicate-submit tests passed.');

for(const html of [local,cloud]){
 const status={state:'SAFE',hardwareRevision:'two-mq-ads1115-v2',home:{home:'Test',address:''},frs:0,grs:0,setup:false,cloudSms:{enabled:false},lastSeen:1,cloudConnected:true};
 const f=fixture(html,()=>reply(200,JSON.stringify(status)));
 await vm.runInContext('refresh()',f.context);
 assert.equal(f.node('capture-photo').disabled,true); // old device cannot accept capture commands
 if(html===cloud)assert.equal(f.node('cloud-sms-test').disabled,true);
 status.cameraReady=true;status.cloudSms.enabled=true;
 await vm.runInContext('refresh()',f.context);
 assert.equal(f.node('capture-photo').disabled,false);
 if(html===cloud)assert.equal(f.node('cloud-sms-test').disabled,false);
 f.cleanup();
}
console.log('Local/cloud snapshot firmware gating and AWS SMS deployment-state controls passed.');
