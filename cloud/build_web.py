from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'firmware/main/web_page.h').read_text().split('R"FG(',1)[1].rsplit(')FG";',1)[0]
# Bootstrap Wi-Fi, bot token and cloud device secret remain on the local setup page.
a=s.index('<fieldset><legend><span class="step">01</span>');b=s.index('</fieldset>',a)+len('</fieldset>')
s=s[:a]+'''<fieldset><legend><span class="step">01</span> Resident access</legend><p class="hint">Configure Wi-Fi, Telegram and the device connection through the ESP32 setup hotspot.</p><div class="grid"><label>Resident join PIN<input name="pin" minlength="6" maxlength="32" required></label></div></fieldset>'''+s[b:]
s=s.replace("['ssid','password','token','pin','home','landmark','station','stationPhone','ambulancePhone','emergencyPhone','emergencyPin']", "['pin','home','landmark','station','stationPhone','ambulancePhone','emergencyPhone','emergencyPin']")
s=s.replace("if(form.elements.password)form.elements.password.value='';if(form.elements.token)form.elements.token.value='';",'')
s=s.replace("$('updated').textContent='Updated just now';$('connection').textContent='Device connected';$('connection').classList.remove('offline');", "$('updated').textContent='Last received: '+new Date(s.lastSeen*1000).toLocaleTimeString();$('connection').textContent=s.cloudConnected?'ESP32 connected':'ESP32 offline';$('connection').classList.toggle('offline',!s.cloudConnected);")
s=s.replace("const s=await r.json();lastGoodAt=Date.now();lastImageAt=s.cameraImageAt||0;const captureButton=$('capture-photo');if(!captureButton.dataset.busy){captureButton.disabled=s.cameraReady===undefined;captureButton.textContent=s.cameraReady===undefined?'Update device firmware to capture':'Capture and send photo';}csrf=", "const s=await r.json();lastGoodAt=Date.now();lastImageAt=s.cameraImageAt||0;const captureButton=$('capture-photo');if(!captureButton.dataset.busy){captureButton.disabled=s.cameraReady===undefined;captureButton.textContent=s.cameraReady===undefined?'Update device firmware to capture':'Capture and send photo';}const smsButton=$('cloud-sms-test');if(smsButton){smsButton.disabled=!s.cloudSms?.enabled;smsButton.textContent=s.cloudSms?.enabled?'AWS test SMS':'AWS SMS is off';smsButton.title=s.cloudSms?.enabled?'Send a text-only SMS test':'AWS SNS SMS is disabled in this deployment';}if(s.waitingForDevice){$('state').textContent='Waiting for ESP32 connection';$('connection').textContent='ESP32 not connected';$('connection').classList.add('offline');$('updated').textContent='Connect the device through its setup hotspot';setTimeout(refresh,5000);return;}csrf=")
s=s.replace("$('state').textContent=s.state;","$('state').textContent=s.cloudConnected?s.state:s.state+' · device offline, last known state';")
s=s.replace('setTimeout(refresh,2000)', 'setTimeout(refresh,5000)')
a=s.index('<details class="training">');b=s.index('</details>',a)+len('</details>');s=s[:a]+s[b:]
s=s.replace('Blank password and bot token fields keep the saved values.','Changes are queued for the ESP32; it validates and applies them when connected.')
s=s.replace('Changes save on this device.','Changes are sent to your connected ESP32.')
s=s.replace('<button id="test">','<button id="cloud-readings">Send readings</button><button id="cloud-sms-test">AWS test SMS</button><button id="test">')
s=s.replace('refresh();\n</script>', '''$('cloud-sms-test').onclick=async()=>{try{$('message').textContent='Sending AWS SMS test…';const r=await deviceRequest('/api/sms-test',{method:'POST',headers:{'X-FireGuard':csrf}});const b=await r.json();$('message').textContent=b.message||b.error;}catch(e){$('message').textContent='Connection lost';}};
$('cloud-readings').onclick=async()=>{const b=$('cloud-readings');b.disabled=true;$('message').textContent='Requesting readings…';try{const r=await deviceRequest('/api/readings',{method:'POST',headers:{'X-FireGuard':csrf}});$('message').textContent=await r.text();}catch(error){$('message').textContent=error.message;}finally{b.disabled=false;}};

refresh();
</script>''')
login='''<div id="cloud-login" style="position:fixed;inset:0;z-index:10;background:#f5f7f2;display:grid;place-items:center;padding:24px"><form id="cloud-login-form" class="panel" style="width:min(100%,400px)"><div class="eyebrow">FireGuard · AWS</div><h1 style="font-size:28px">Your home, connected.</h1><p class="hint">Enter your dashboard access key to view your device and manage settings.</p><label>Dashboard access key<input id="access-key" type="password" required autocomplete="off"></label><button class="primary" style="margin-top:20px;width:100%">Open FireGuard →</button><p id="login-error" class="hint"></p></form></div>
<script>
const nativeFetch=window.fetch.bind(window);let accessKey=sessionStorage.getItem('fireguard-access')||'';
const login=document.getElementById('cloud-login');if(accessKey)login.style.display='none';
window.fetch=async(input,options={})=>{const headers=new Headers(options.headers||{});headers.set('Authorization','Bearer '+accessKey);const r=await nativeFetch(input,{...options,headers});if(r.status===401){login.style.display='grid';document.getElementById('login-error').textContent=accessKey?'Access key not accepted.':'';}return r;};
document.getElementById('cloud-login-form').onsubmit=async e=>{e.preventDefault();const value=document.getElementById('access-key').value.trim();const r=await nativeFetch('/api/status',{headers:{Authorization:'Bearer '+value}});if(!r.ok){document.getElementById('login-error').textContent='Access key not accepted.';return;}sessionStorage.setItem('fireguard-access',value);location.reload();};
</script>'''
s=s.replace('<div class="topbar">',login+'<div class="topbar">',1)
a=s.index('<fieldset><legend><span class="step">05</span>');b=s.index('</fieldset>',a)+len('</fieldset>');s=s[:a]+s[b:]
# SMS (textbee) API key is configured only on the local setup hotspot, like the Telegram token.
a=s.index('<fieldset><legend><span class="step">06</span>');b=s.index('</fieldset>',a)+len('</fieldset>');s=s[:a]+s[b:]
s=s.replace("data.cloudEnabled=form.elements.cloudEnabled.checked;data.cloudUrl=form.elements.cloudUrl.value;data.cloudDeviceToken=form.elements.cloudDeviceToken.value;",'')
s=s.replace("if(form.elements.cloudDeviceToken)form.elements.cloudDeviceToken.value='';",'')
s=s.replace("Saved photos: ${s.samples}","Last command: ${s.lastCommandResult||'none'}\\nAWS SMS: ${s.cloudSms?.enabled?'on':'off'}${s.cloudSms?.last?' · last: '+s.cloudSms.last.kind+' sent '+s.cloudSms.last.sent+', failed '+s.cloudSms.last.failed+' at '+new Date(s.cloudSms.last.at*1000).toLocaleTimeString():''}\\nSaved photos: ${s.samples}")
# Hybrid-location controls (Google API key, Wi-Fi detect, fire-station SMS switch/test) are device-local.
import re
s=re.sub(r'<!--local-only-->.*?<!--/local-only-->','',s,flags=re.S)
(root/'cloud/web/index.html').write_text(s)
