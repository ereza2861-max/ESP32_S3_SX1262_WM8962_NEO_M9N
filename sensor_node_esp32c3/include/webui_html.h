#pragma once

// WebUI HTML for the ESP32-C3 sensor node OTA AP.
// Embedded in PROGMEM to avoid SPIFFS dependency.
//
// SECURITY (per C3 mitigation): no OTA password can be set over this open AP.
// The upload form requires the OTA password already provisioned over serial.

static const char WEBUI_HTML_PROGMEM[] PROGMEM = R"HTML(<!doctype html>
<html lang=en><head><meta charset=utf-8><title>FieldRadio Sensor OTA</title>
<style>
body{font-family:sans-serif;max-width:40rem;margin:2rem auto;padding:0 1rem}
h1{font-size:1.2rem}fieldset{margin:1rem 0;padding:.5rem 1rem}
label{display:block;margin:.25rem 0}input,button{font-size:1rem;padding:.3rem}
pre{background:#eee;padding:.5rem;overflow:auto}
</style></head><body>
<h1>FieldRadio Sensor Node</h1>
<fieldset><legend>Profile</legend>
<label>Runtime profile <select id=profile><option value=0>0 — Island / Sea</option><option value=1>1 — Tropical Forest</option><option value=2>2 — Volcanic Mountain</option><option value=3>3 — Sub-Zero Snow</option><option value=4>4 — Desert</option><option value=5>5 — Mine Tunnel</option></select></label>
<label>OTA password <input id=profilePassword type=password required></label>
<button id=applyProfile type=button>Apply profile</button>
<pre id=profileResult></pre></fieldset>
<fieldset><legend>Status</legend><pre id=status>loading...</pre></fieldset>
<fieldset><legend>Firmware upload</legend>
<p>Upload requires the OTA password provisioned over serial.</p>
<form id=up method=POST action=/update enctype=multipart/form-data>
<label>OTA password <input type=password name=password required></label>
<label>Firmware (.bin) <input type=file name=firmware accept=.bin required></label>
<button type=submit>Upload</button>
</form>
<pre id=result></pre></fieldset>
<fieldset><legend>AP</legend>
<p>AP auto-shuts down 10 minutes after AP start; client activity does not extend the window.
Toggling the AP is only possible with the physical button long-press.</p>
</fieldset>
<script>
let otaMeta={};
function hexBytes(hex){
  const out=new Uint8Array(hex.length/2);
  for(let i=0;i<out.length;i++) out[i]=parseInt(hex.substr(i*2,2),16);
  return out;
}
function nonceHex(){
  const b=new Uint8Array(16); crypto.getRandomValues(b);
  return [...b].map(x=>x.toString(16).padStart(2,'0')).join('');
}
async function refresh(){
  try{
    const r=await fetch('/status',{cache:'no-store'}); const j=await r.json();
    otaMeta=j; status.textContent=JSON.stringify(j,null,2);
  }catch(e){status.textContent='status unavailable';}
}
async function signingKey(password){
  const base=await crypto.subtle.importKey('raw',new TextEncoder().encode(password),
    'PBKDF2',false,['deriveBits']);
  const bits=await crypto.subtle.deriveBits(
    {name:'PBKDF2',salt:hexBytes(otaMeta.signingSalt),
     iterations:Number(otaMeta.signingIterations),hash:'SHA-256'},base,256);
  return crypto.subtle.importKey('raw',bits,{name:'HMAC',hash:'SHA-256'},false,['sign']);
}
async function signBody(password,nonce,bytes){
  const key=await signingKey(password);
  const n=hexBytes(nonce), data=new Uint8Array(n.length+bytes.length);
  data.set(n,0); data.set(bytes,n.length);
  const sig=await crypto.subtle.sign('HMAC',key,data);
  return [...new Uint8Array(sig)].map(x=>x.toString(16).padStart(2,'0')).join('');
}
refresh();setInterval(refresh,5000);
up.addEventListener('submit',async(e)=>{
  e.preventDefault(); result.textContent='signing/uploading...';
  try{
    if(!otaMeta.signingSalt) await refresh();
    const file=up.querySelector('input[type=file]').files[0];
    const password=up.querySelector('input[name=password]').value;
    if(!file) throw new Error('firmware file required');
    const nonce=nonceHex(), bytes=new Uint8Array(await file.arrayBuffer());
    const signature=await signBody(password,nonce,bytes);
    const form=new FormData(up);
    const r=await fetch('/update',{method:'POST',
      headers:{'X-OTA-Nonce':nonce,'X-OTA-Signature':signature,
               'X-OTA-Session':otaMeta.session},body:form});
    result.textContent=await r.text();
  }catch(e){result.textContent='upload failed: '+e.message;}
});
applyProfile.addEventListener('click',async()=>{
  if(!confirm('Confirm: physical sensor cables have been disconnected from the old profile and connected for the new profile. Continue?')) return;
  try{
    if(!otaMeta.signingSalt) await refresh();
    const password=profilePassword.value;
    const body=JSON.stringify({profile:Number(profile.value)});
    const nonce=nonceHex();
    const signature=await signBody(password,nonce,new TextEncoder().encode(body));
    const r=await fetch('/profile',{method:'POST',
      headers:{'Content-Type':'application/json','X-Profile-Cables-Changed':'true',
               'X-OTA-Password':password,'X-OTA-Session':otaMeta.session,
               'X-OTA-Nonce':nonce,'X-OTA-Signature':signature},body});
    profileResult.textContent=await r.text();
  }catch(e){profileResult.textContent='profile change request failed: '+e.message;}
});
</script>
</body></html>)HTML";
