#include "WebUi.h"
#include "generated/WebTlsProvisioning.h"
#include "Config.h"
#include "AppState.h"
#include "SensorSpool.h"
#include "StorageManager.h"
#include "LoRaManager.h"
#include "LoRaWANManager.h"
#include "AudioManager.h"
#include "PersistentConfig.h"
#include "WebUiNumericParser.h"
#include "WebUiSessionPolicy.h"
#include "MqttClientManager.h"
#include "BleSensorReader.h"
#include <cstring>
#include <WiFi.h>
#include <SD.h>
#include <Preferences.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <mbedtls/aes.h>
#ifndef CONFIG_SECURE_BOOT_V2_ENABLED
#define CONFIG_SECURE_BOOT_V2_ENABLED 0
#endif
#ifndef CONFIG_SECURE_FLASH_ENC_ENABLED
#define CONFIG_SECURE_FLASH_ENC_ENABLED 0
#endif
#include <esp_system.h>
#include <nvs_flash.h>
#include <ctype.h>
#include <memory>

extern SensorSpool sensorSpool;
extern CertLifecycleManager certLifecycle;

// ---------------------------------------------------------------------------
// Free helpers (preserved from the pre-migration WebUi.cpp)
// ---------------------------------------------------------------------------

static String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(input[i]);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          const char hex[] = "0123456789ABCDEF";
          out += "\\u00";
          out += hex[(c >> 4) & 0x0F];
          out += hex[c & 0x0F];
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

static String hexEncodeUi(const uint8_t* data, size_t len) {
  static const char h[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += h[data[i] >> 4];
    out += h[data[i] & 0x0f];
  }
  return out;
}

static bool hexDecodeUi(const String& in, uint8_t* out, size_t len) {
  if (!out || in.length() != len * 2) return false;
  auto n = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = n(in[i * 2]), lo = n(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

// ---------------------------------------------------------------------------
// INDEX_HTML
// ---------------------------------------------------------------------------
// The embedded UI page is unchanged. See the pre-migration WebUi.cpp for the
// exact body; it is included here verbatim so the migration stays a pure
// server-architecture change.
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>FieldRadio</title><style>
body{font-family:sans-serif;max-width:1100px;margin:auto;padding:16px;background:#111;color:#eee}
body.light{background:#f5f5f5;color:#111} body.light .card{border-color:#bbb} body.light pre{background:#e8e8e8}
.card{border:1px solid #444;border-radius:8px;padding:12px;margin:8px 0}
@media(max-width:600px){body{padding:8px}.card{padding:8px}button,input,select{width:100%;box-sizing:border-box;margin:3px 0}table{font-size:.8rem;display:block;overflow-x:auto}}
button,input{font-size:1rem;margin:4px;padding:10px}pre{background:#222;padding:10px;overflow:auto}
.battery-low{outline:3px solid orange}.battery-critical{outline:4px solid red}.sensor-badge{padding:2px 5px;border-radius:4px;font-size:.75rem;border:1px solid #777}.sensor-badge.q0{background:#164d25}.sensor-badge:not(.q0){background:#6a4b00}
.map-shell{height:420px;min-height:280px;border-radius:6px;overflow:hidden;background:#222}
.map-status{min-height:1.4em;margin:4px 0}
@media(max-width:600px){.map-shell{height:55vh;min-height:300px}}
</style></head><body class="__THEME_CLASS__"><h1 id=title>FieldRadio</h1><label>Language <select id=lang onchange="setLang()"><option value="en">EN</option><option value="id">ID</option></select></label>
<div class=card>
<button onclick="ptt(1)">PTT ON</button><button onclick="ptt(0)">PTT OFF</button>
<button onclick="sos(1)">SOS</button><button onclick="sos(0)">Cancel SOS</button><span id=sosBadge></span><button onclick="refreshSosHistory()">SOS history</button><button onclick="rec(1)">REC</button>
<button onclick="rec(0)">STOP REC</button><button onclick="recordPause(1)">PAUSE REC</button><button onclick="recordPause(0)">RESUME REC</button><button onclick="recordSplit()">SPLIT REC</button>
<label>Record source
<select id=audsrc onchange="setAudioSource()">
<option value="0">WM8962 MIC</option>
<option value="1">LINE-IN 2</option>
<option value="2">LINE-IN 3</option>
<option value="3">USB Audio</option>
</select></label>
<button onclick="play()">PLAY</button><button onclick="pausePlay(1)">PAUSE</button><button onclick="pausePlay(0)">RESUME</button><button onclick="stopPlay()">STOP</button><input id=seekms type=number value="0" min="0"><button onclick="seekPlay()">SEEK ms</button><button onclick="queue()">QUEUE</button><button onclick="clearQueue()">CLEAR QUEUE</button>
<button onclick="tone(880,120)">BEEP</button>
<label>USB monitor <input id=usbmon type=checkbox onchange="setUsbMonitor()"></label><label>SD→USB <input id=usbtransport type=checkbox onchange="setUsbTransport()"></label>
<label>Loopback <input id=loop type=checkbox onchange="setLoopback()"></label><label>VOX threshold <input id=voxThreshold type=number step="0.005" min="0.005" max="1" value="0.08"></label><label>hang ms <input id=voxHang type=number min="50" max="10000" value="700"></label><label>AEC <input id=aec type=checkbox onchange="setAec()"></label><label>VOX <input id=vox type=checkbox onchange="setVox()"></label><label>Record quality <select id=recordQuality onchange="setRecordQuality()"><option value="low">8k mono</option><option value="medium">16k mono</option><option value="high" selected>44.1k stereo</option></select></label>
</div>
<div class=card><input id=msg placeholder="LoRa message">
<button onclick="send()">Send</button></div><div class=card><h3>Messages <span id=unreadBadge>0 unread</span></h3>
<input id=messageQuery placeholder="Search text/source ID">
<input id=messageFrom type=number placeholder="From epoch"><input id=messageTo type=number placeholder="To epoch">
<button onclick="refreshMessages()">Search</button><button onclick="readAllMessages()">Mark all read</button>
<button onclick="clearMessages()">Delete history</button>
<button onclick="exportMessages('csv')">Export CSV</button><button onclick="exportMessages('json')">Export JSON</button>
<pre id=messages></pre><input id=replySource placeholder="Reply source ID"><input id=replyText placeholder="Reply text">
<button onclick="replyMessage()">Reply</button></div>
<div class=card><input id=file value="/REC/">
<button onclick="play()">Play WAV</button></div>
<div class=card><h3>LoRaWAN</h3>
<label>Region <select id=lwRegion><option value="0">AS923-1</option><option value="1" selected>AS923-2 (Indonesia)</option><option value="2">AS923-3</option><option value="3">AS923-4</option></select></label>
<label>Mode <select id=lwMode><option value="0">OTAA</option><option value="1">ABP</option></select></label>
<input id=lwDevEui placeholder="DevEUI 16 hex">
<input id=lwJoinEui placeholder="JoinEUI 16 hex (OTAA)">
<input id=lwAppKey placeholder="AppKey 32 hex (OTAA)" type=password>
<input id=lwDevAddr placeholder="DevAddr 8 hex (ABP)">
<input id=lwNwkSKey placeholder="NwkSKey 32 hex (ABP)" type=password>
<input id=lwAppSKey placeholder="AppSKey 32 hex (ABP)" type=password>
<input id=lwFPort type=number min=1 max=223 value=1 placeholder="FPort">
<input id=lwPeriod type=number min=30 max=86400 value=300 placeholder="Uplink period seconds">
<button onclick="saveLw()">Save LoRaWAN</button><button onclick="lwConnect()">Connect</button><button onclick="lwDisconnect()">Disconnect</button>
<button onclick="lwUplink()">Kirim Uplink Test</button><textarea id=lwPayload maxlength=51 placeholder="Payload text or hex via prefix hex:"></textarea>
<pre id=lwStatus></pre></div>
<div class=card><h3>Configuration</h3>
<input id=freq value="923" placeholder="Freq MHz"><input id=bw value="125" placeholder="BW kHz">
<input id=sf value="7" placeholder="SF"><input id=cr value="5" placeholder="CR 5-8">
<input id=pwr value="14" placeholder="Power dBm"><input id=sw value="18" placeholder="Sync word">
<input id=cs value="FIELD" placeholder="Callsign"><input id=key placeholder="LoRa AES-128 key (32 hex chars)"><input id=vol value="70" placeholder="Volume">
<input id=bat value="1.0" placeholder="Battery calibration">
<input id=batActual placeholder="Actual battery voltage, e.g. 3.95"><button onclick="calBattery()">CALIBRATE BATTERY</button>
<input id=staSsid value="" maxlength="32" placeholder="STA SSID"><input id=staPassword value="" maxlength="63" type=password placeholder="STA password (8-63)"><input id=aps value="" placeholder="AP password"><input id=wp value="" placeholder="Web password">
<button onclick="saveCfg()">Save config</button><button onclick="reboot()">Reboot</button><button onclick="factoryReset()">Factory reset</button></div>
<div class=card><h3>Runtime Advanced Settings</h3>
<label>MQTT enabled <input id=mqttEnabled type=checkbox checked></label>
<label>Time-sync wake period (seconds) <input id=wakePeriodSec type=number min=60 max=604800 value=43200></label>
<label>Automatic deep-sleep <input id=deepSleepEnabled type=checkbox checked></label>
<label>Deep-sleep idle timeout (seconds) <input id=deepSleepIdleSec type=number min=60 max=86400 value=300></label>
<label>Wake grace period (ms) <input id=deepSleepWakeGraceMs type=number min=100 max=60000 value=5000></label>
<label>Critical-battery shutdown delay (ms) <input id=criticalShutdownDelayMs type=number min=100 max=600000 value=1500></label>
<label>Battery low threshold (V) <input id=batteryLowThreshold type=number step="0.01" min=2.5 max=4.2 value=3.4></label>
<label>Battery critical threshold (V) <input id=batteryCriticalThreshold type=number step="0.01" min=2.5 max=4.2 value=3.2></label>
<label>Class-D speaker enabled <input id=classDEnabled type=checkbox disabled></label>
<label>Class-D boost (0..7) <input id=classDBoost type=number min=0 max=7 value=0></label>
<label>MQTT host <input id=mqttHost maxlength=253></label><label>MQTT port <input id=mqttPort type=number min=1 max=65535></label>
<label>MQTT TLS required <input id=mqttTls type=checkbox checked__MQTT_TLS_DISABLED__></label>
<label>MQTT reconnect min/max ms <input id=mqttRetryMin type=number min=1000><input id=mqttRetryMax type=number min=1000></label>
<label>MQTT telemetry/health ms <input id=mqttTelemetry type=number min=1000><input id=mqttHealth type=number min=1000></label>
<small>MQTT credential rotation days is compatibility metadata only; it never triggers automatic credential rotation.</small> <label>Retain telemetry <input id=mqttRetainTelemetry type=checkbox></label><label>Retain availability/LWT <input id=mqttRetainAvailability type=checkbox checked></label>
<div class=card><h4>Certificate Lifecycle (EST / PKI)</h4>
<label>Lifecycle enabled <input id=certLifecycleEnabled type=checkbox></label>
<label>EST server URL <input id=estServerUrl maxlength=253 placeholder="https://est.example.com:8443"></label>
<label>EST label <input id=estLabel maxlength=95 value="/.well-known/est"></label>
<label>Renewal threshold (days) <input id=certRenewalThresholdDays type=number min=1 max=3650 value=30></label>
<label>Check period (ms) <input id=certCheckPeriodMs type=number min=3600000 value=86400000></label>
<label>EST auth mode <select id=estAuthMode><option value=0>Factory bootstrap certificate</option><option value=1>Basic Auth</option><option value=2>Bootstrap Token</option></select></label>
  <label>EST username (mode 1) <input id=estUsername maxlength=64 autocomplete="off"></label>
  <label>EST password (mode 1) <input id=estPassword type=password maxlength=64 autocomplete="new-password"></label>
  <label>EST bootstrap token (mode 2) <input id=estBootstrapToken type=password maxlength=128 autocomplete="off"></label>
<button onclick="renewCert()">Renew Now</button><button onclick="fetchCertCa()">Fetch CA Chain</button>
<pre id=certStatus></pre><pre id=certHistory></pre></div>
<label>BLE reader enabled <input id=bleEnabled type=checkbox checked></label><label>BLE pairing <input id=blePairing type=checkbox checked></label>
<label>BLE scan interval/window ms <input id=bleScanInterval type=number min=100><input id=bleScanWindow type=number min=1></label>
<label>BLE scan duration/connect timeout ms <input id=bleScanDuration type=number min=100><input id=bleConnectTimeout type=number min=500></label>
<label>BLE eviction ms <input id=bleEviction type=number min=10000></label><label>BLE max nodes <input id=bleMaxNodes type=number min=1 max=8></label>
<label>BLE encryption required <input id=bleEncryption type=checkbox checked></label><label>BLE pairing failure threshold <input id=bleFailureThreshold type=number min=1 max=20></label>
<label>BLE pairing block ms <input id=bleBlockMs type=number min=1000></label><label>BLE keep-awake <input id=bleKeepAwake type=checkbox checked></label>
<label>ADR enabled <input id=adr type=checkbox></label><label>HOP enabled <input id=hop type=checkbox></label><label>HOP channel profile <input id=hopProfile type=number min=1 max=8></label>
<label>Range-test mode <input id=rangeMode type=checkbox></label>
<label>Web session timeout ms <input id=webSessionTimeout type=number min=60000></label><label>Auth rate-limit ms <input id=webRateLimit type=number min=100></label>
<label>CSRF policy <select id=csrfPolicy><option value=0>token + Origin</option><option value=1>token only</option></select></label>
<label>BLE pairing policy <select id=blePairingPolicy><option value=0>default</option><option value=1>strict</option></select></label>
<label>ECDH rekey policy <select id=ecdhPolicy><option value=0>off</option><option value=1>required</option></select></label>
<label>Replay window bits (8..32) <input id=replayWindow type=number min=8 max=32></label>
<small>Class-D output mode, mono/stereo, speaker impedance and SPKVDD are hardware-contract values and are not runtime controls.</small>
</div>
<div class=card><h3>Radio diagnostics</h3>
<input id=tuneFreq type=number step="0.001" min="920" max="923" placeholder="Frequency MHz">
<button onclick="tuneRadio()">Manual tune</button><button onclick="refreshRadioHistory()">Refresh RSSI/SNR history</button><button onclick="refreshDiagnostics()">Refresh diagnostics</button><button onclick="captureStart()">Capture 10s</button><button onclick="captureStop()">Stop capture</button><button onclick="captureDump()">Dump capture</button><button onclick="toggleAdr()">ADR</button><button onclick="selfTest()">Self-test</button>
<pre id=radioHistory></pre><pre id=radioStats></pre><div id=storageInfo></div><div id=diagnostics></div></div>
<div class=card><h3>Channel Scanner</h3>
<button onclick="scanStart(1)">Scan Once</button>
<button onclick="scanStart(2)">Scan Continuous</button>
<button onclick="scanStop()">Stop Scan</button>
<button onclick="hopSuggest()">Suggest Hop Channels</button>
<button onclick="hopEnable(1)">Enable Hop</button>
<button onclick="hopEnable(0)">Disable Hop</button>
<input id=hopList placeholder="0,2,5"><button onclick="hopSetChannels()">Set channels</button>
<button onclick="rangeTest(1)">Range test ON</button><button onclick="rangeTest(0)">Range test OFF</button>
<pre id=scanmsg></pre>
<div id=scanresults><table><thead><tr>
<th>Freq MHz</th><th>RSSI avg</th><th>RSSI peak</th><th>SNR</th>
<th>Occupancy</th><th>Preamble</th><th>Load</th>
</tr></thead><tbody id=scantbody></tbody></table></div>
<div id=hopsummary></div>
</div>
<div class=card id=sensorPanel><h3>BLE Sensor Nodes</h3>
<table><thead><tr><th>Node</th><th>Address</th><th>RSSI</th><th>Sensors</th><th>Last Seen</th><th>Status</th><th>Actions</th></tr></thead><tbody id=sensorNodesBody></tbody></table>
<div id=sensorDetail></div></div>
<div class=card><button onclick="toggleTheme()">Dark/light</button><button onclick="showTrack()">Map</button><span id=toast></span></div>
<div class=card id=trackPanel style="display:none"><h3>Node Primer Map</h3>
<label>Historical Marker <input id=historicalMarker type=checkbox onchange="toggleHistoricalMarker()"></label>
<button onclick="refreshMap()">Refresh map</button><div id=mapStatus class=map-status>Map not initialized</div>
<div id=trackMap class=map-shell></div>
<small>Historical visualization displays up to 1000 markers; the stored track remains available through the existing track download.</small></div>
<div class=card><h3>Status</h3><pre id=s></pre></div>
<div class=card><h3>Files</h3><input id=fileDir value="/REC/"><button onclick="refreshFiles()">Open folder</button><pre id=f></pre>
<input id=upfile type=file accept=".wav,.WAV"><button onclick="uploadFile()">UPLOAD WAV</button>
<input id=renameFrom placeholder="/REC/old.WAV"><input id=renameTo placeholder="/REC/new.WAV"><button onclick="renameFile()">RENAME</button>
<a id=trackDownload href="/api/track/download">Download GPS track</a>
</div>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" integrity="sha256-p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY=" crossorigin="">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js" integrity="sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo=" crossorigin=""></script>
<script>
const CSRF_TOKEN='__CSRF_TOKEN__';
let trackMap=null,liveLayer=null,historicalLayer=null,currentMarker=null;
const neighborMarkers=new Map();
let mapInitialized=false,historicalLoadToken=0,lastLiveBoundsKey='';
const historicalMarkerControl=document.getElementById('historicalMarker');
function sensorBadge(q){let a=[];if(q&1)a.push('STALE');if(q&2)a.push('RANGE');if(q&4)a.push('GW-TS');if(q&8)a.push('BAD-TS');if(q&16)a.push('LINK');return `<span class="sensor-badge q${q}">${a.length?a.join(' '):'VALID'}</span>`}
function sensorEscape(v){return String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function sensorAction(id,action){if(!confirm(action==='forget'?'Forget this sensor node?':'Reconnect this sensor node?'))return;try{const r=await fetch(`/api/sensors/${action}?id=${encodeURIComponent(id)}`,{method:'POST',headers:{'X-CSRF-Token':CSRF_TOKEN}});toast(await r.text());await refreshSensorNodes()}catch(e){toast('Sensor action failed')}}
async function refreshSensorDetail(id){try{const n=await (await fetch('/api/sensors/nodes?id='+encodeURIComponent(id))).json();if(!n.ok){sensorDetail.textContent=n.error||'Node unavailable';return}let h=`<h4>Node ${n.id} — ${sensorEscape(n.name||'(unnamed)')}</h4><table><thead><tr><th>ID</th><th>Name</th><th>Unit</th><th>Value</th><th>Timestamp</th><th>Quality</th></tr></thead><tbody>`;for(const x of n.sensors||[]){h+=`<tr><td>${x.id}</td><td>${sensorEscape(x.name)}</td><td>${sensorEscape(x.unit)}</td><td>${x.valueValid?x.value:'—'}</td><td>${x.timestamp||'—'}</td><td>${sensorBadge(x.quality)}</td></tr>`}sensorDetail.dataset.nodeId=String(n.id);sensorDetail.innerHTML=h+'</tbody></table>'}catch(e){toast('Sensor detail failed')}}
async function refreshSensorNodes(){try{const a=await (await fetch('/api/sensors/nodes')).json();sensorNodesBody.innerHTML=(a.nodes||[]).map(n=>`<tr><td><button onclick="refreshSensorDetail(${n.id})">${n.id}</button></td><td>${sensorEscape(n.address)}</td><td>${n.rssi}</td><td>${n.sensorCount}</td><td>${n.lastSeenMs} ms</td><td>${n.connected?'CONNECTED':'OFFLINE'}</td><td><button onclick="sensorAction(${n.id},'refresh')">Refresh</button><button onclick="sensorAction(${n.id},'forget')">Forget</button></td></tr>`).join('')}catch(e){toast('Sensor inventory failed')}}
async function refreshSensorLive(){try{const a=await (await fetch('/api/sensors/live')).json();if(a.nodes) for(const n of a.nodes){const open=document.getElementById('sensorDetail');if(open.dataset.nodeId==n.id) await refreshSensorDetail(n.id)}}catch(e){}}
refreshSensorNodes();setInterval(refreshSensorNodes,5000);setInterval(refreshSensorLive,2000);

async function j(u,o={}){o.headers=Object.assign({},o.headers||{},o.method&&o.method.toUpperCase()!=='GET'?{'X-CSRF-Token':CSRF_TOKEN}:{});let r=await fetch(u,o);return await r.text()}
function toast(t){document.getElementById('toast').textContent=t;setTimeout(()=>document.getElementById('toast').textContent='',2500)}
async function setLang(){const c=document.getElementById('lang').value;localStorage.setItem('fieldradio-lang',c);try{await fetch('/api/lang?set='+c,{method:'POST',headers:{'X-CSRF-Token':csrfToken}})}catch(e){};document.getElementById('title').textContent=c==='id'?'FieldRadio':'FieldRadio'}
if(localStorage.getItem('fieldradio-lang'))document.getElementById('lang').value=localStorage.getItem('fieldradio-lang');
async function toggleTheme(){const m=document.body.classList.toggle('light')?'light':'dark';localStorage.setItem('fieldradio-theme',m);try{await fetch('/api/theme?mode='+m,{method:'POST',headers:{'X-CSRF-Token':'__CSRF_TOKEN__'}})}catch(e){}}
if(localStorage.getItem('fieldradio-theme')==='light')document.body.classList.add('light');
async function refreshMessages(){
 try{
  const q=new URLSearchParams(); if(messageQuery.value)q.set('q',messageQuery.value);
  if(messageFrom.value)q.set('from',messageFrom.value); if(messageTo.value)q.set('to',messageTo.value);
  const a=await (await fetch('/api/messages?'+q)).json();
  messages.textContent=a.map(x=>`${x.read?'':'[UNREAD] '}ts=${x.ts} source=${x.source}: ${x.text}`).join('\\n');
 }catch(e){toast('Message refresh failed')}
}
async function readAllMessages(){await j('/api/messages/read?ts=all',{method:'POST'});refresh()}
async function clearMessages(){if(!confirm('Delete message history?'))return;await j('/api/messages/clear',{method:'POST'});refreshMessages();refresh()}
async function replyMessage(){const r=await j('/api/messages/reply?source='+encodeURIComponent(replySource.value),{method:'POST',headers:{'Content-Type':'text/plain'},body:replyText.value});toast(r);refresh()}
function exportMessages(f){window.location='/api/messages/export?format='+f}
async function tuneRadio(){const r=await j('/api/radio/tune?freq='+encodeURIComponent(tuneFreq.value),{method:'POST'});toast(r)}
async function refreshDiagnostics(){
 try{
  const n=await (await fetch('/api/neighbors')).json();
  const r=await (await fetch('/api/routes')).json();
  const d=await (await fetch('/api/dedup/stats')).json();
  diagnostics.textContent=JSON.stringify({neighbors:n,routes:r,dedup:d},null,2);
 }catch(e){toast('Diagnostics refresh failed')}
}
async function captureStart(){toast(await j('/api/capture/start?duration=10000',{method:'POST'}))}
async function captureStop(){toast(await j('/api/capture/stop',{method:'POST'}))}
async function captureDump(){try{diagnostics.textContent=JSON.stringify(await (await fetch('/api/capture/dump')).json(),null,2)}catch(e){toast('Capture dump failed')}}
async function toggleAdr(){toast(await j('/api/adr?on=1',{method:'POST'}));refresh()}
async function selfTest(){try{diagnostics.textContent=JSON.stringify(await (await fetch('/api/selftest',{method:'POST'})).json(),null,2)}catch(e){toast('Self-test failed')}}
async function refreshRadioHistory(){
 try{const a=await (await fetch('/api/radio/history')).json();radioHistory.textContent=a.map(x=>`+${x.ms}ms RSSI=${x.rssi} SNR=${x.snr}`).join('\\n');
 const st=await (await fetch('/api/storage/info')).json();storageInfo.textContent=`SD used ${st.used} / ${st.total} bytes (${st.free} free)`;
 const rs=await (await fetch('/api/radio/stats')).json(); radioStats.textContent=JSON.stringify(rs,null,2);
 radioHistory.textContent+='\\nSTATS RSSI avg='+rs.rssiAvg+' range=['+rs.rssiMin+','+rs.rssiMax+'] SNR avg='+rs.snrAvg;
 }catch(e){toast('Radio/storage refresh failed')}
}
function setMapStatus(t){document.getElementById('mapStatus').textContent=t}
function popupNode(sourceId,rssi,quality,epoch){
  const d=document.createElement('div');
  const title=document.createElement('strong'); title.textContent='Connected node '+String(sourceId); d.appendChild(title);
  const meta=document.createElement('div'); meta.textContent=`RSSI ${rssi} dBm, quality ${quality}`;
  d.appendChild(meta);
  if(epoch) { const ts=document.createElement('div'); ts.textContent='Location timestamp '+String(epoch); d.appendChild(ts); }
  return d;
}
function initMap(){
  if(mapInitialized) return true;
  if(typeof L==='undefined'){setMapStatus('Leaflet failed to load; map controls unavailable');return false}
  trackMap=L.map('trackMap');
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{
    maxZoom:19,attribution:'© OpenStreetMap',crossOrigin:true
  }).addTo(trackMap).on('tileerror',()=>setMapStatus('OpenStreetMap tile unavailable; markers remain available'));
  liveLayer=L.layerGroup().addTo(trackMap);
  historicalLayer=L.layerGroup();
  mapInitialized=true;
  setTimeout(()=>trackMap.invalidateSize(),50);
  return true;
}
function showTrack(){
  const p=document.getElementById('trackPanel');
  p.style.display=p.style.display==='none'?'block':'none';
  if(p.style.display==='block'){
    if(initMap()) { setTimeout(()=>trackMap.invalidateSize(),50); refreshMap(); }
  }
}
function validCoord(lat,lon){
  return Number.isFinite(lat)&&Number.isFinite(lon)&&lat>=-90&&lat<=90&&lon>=-180&&lon<=180;
}
function updateCurrentMarker(gps){
  if(!trackMap) return;
  if(currentMarker){liveLayer.removeLayer(currentMarker);currentMarker=null}
  if(!gps||!gps.valid||!validCoord(Number(gps.lat),Number(gps.lon))){
    return;
  }
  currentMarker=L.marker([Number(gps.lat),Number(gps.lon)]);
  const d=document.createElement('div');
  const title=document.createElement('strong');title.textContent='Node Primer';d.appendChild(title);
  const meta=document.createElement('div');meta.textContent=`${Number(gps.lat).toFixed(6)}, ${Number(gps.lon).toFixed(6)}`;d.appendChild(meta);
  currentMarker.bindPopup(d).addTo(liveLayer);
}
function updateNeighborMarkers(nodes){
  if(!trackMap) return;
  const active=new Set();
  for(const n of (Array.isArray(nodes)?nodes:[])){
    const id=String(n?.sourceId??'');
    if(!id||n.connected!==true) continue;
    const loc=n.location;
    if(!loc||!validCoord(Number(loc.lat),Number(loc.lon))) continue;
    active.add(id);
    let marker=neighborMarkers.get(id);
    const point=[Number(loc.lat),Number(loc.lon)];
    if(!marker){
      marker=L.circleMarker(point,{radius:7});
      neighborMarkers.set(id,marker);
      marker.addTo(liveLayer);
    }else marker.setLatLng(point);
    marker.bindPopup(popupNode(id,Number(n.rssi),Number(n.quality),Number(loc.epoch)||0));
  }
  for(const [id,marker] of neighborMarkers){
    if(!active.has(id)){liveLayer.removeLayer(marker);neighborMarkers.delete(id)}
  }
}
async function refreshMap(){
  if(!initMap()) return;
  try{
    const [gps,nodes]=await Promise.all([
      fetch('/api/track',{cache:'no-store'}).then(r=>r.json()),
      fetch('/api/neighbors',{cache:'no-store'}).then(r=>r.json())
    ]);
    updateCurrentMarker(gps);
    updateNeighborMarkers(nodes);
    const points=[];
    if(gps?.valid&&validCoord(Number(gps.lat),Number(gps.lon))) points.push([Number(gps.lat),Number(gps.lon)]);
    for(const n of (Array.isArray(nodes)?nodes:[])){
      const p=n?.location;
      if(n?.connected===true&&p&&validCoord(Number(p.lat),Number(p.lon))) points.push([Number(p.lat),Number(p.lon)]);
    }
    const boundsKey=points.map(p=>p.map(v=>v.toFixed(6)).join(',')).sort().join(';');
    if(points.length&&boundsKey!==lastLiveBoundsKey){
      trackMap.fitBounds(L.latLngBounds(points),{padding:[20,20],maxZoom:16});
      lastLiveBoundsKey=boundsKey;
    }
    setMapStatus(points.length?`${points.length} live location(s) available`:'No valid live location available');
  }catch(e){setMapStatus('Map data unavailable; existing UI remains usable')}
}
async function loadHistorical(){
  if(!trackMap||!historicalMarkerControl.checked) return;
  const token=++historicalLoadToken;
  try{
    const a=await (await fetch('/api/track/simplified?epsilon=10',{cache:'no-store'})).json();
    if(token!==historicalLoadToken||!historicalMarkerControl.checked)return;
    if(historicalLayer)historicalLayer.clearLayers();
    const points=(Array.isArray(a)?a:[]).filter(x=>validCoord(Number(x.lat),Number(x.lon)));
    if(!points.length){setMapStatus('No historical location available');return}
    const latlng=points.map(x=>[Number(x.lat),Number(x.lon)]);
    L.polyline(latlng).addTo(historicalLayer);
    const markerLimit=1000;
    const step=Math.max(1,Math.ceil(points.length/markerLimit));
    for(let i=0;i<points.length;i+=step)
      L.circleMarker(latlng[i],{radius:4}).bindTooltip(String(points[i].epoch||''),{permanent:false}).addTo(historicalLayer);
    historicalLayer.addTo(trackMap);
    setMapStatus(points.length>markerLimit
      ? `Historical track: ${points.length} points, ${Math.ceil(points.length/step)} markers shown`
      : `Historical track: ${points.length} markers shown`);
    trackMap.fitBounds(L.latLngBounds(latlng),{padding:[20,20],maxZoom:16});
  }catch(e){setMapStatus('Historical data unavailable')}
}
function toggleHistoricalMarker(){
  if(!trackMap||!historicalMarkerControl.checked){
    if(trackMap&&historicalLayer) trackMap.removeLayer(historicalLayer);
    ++historicalLoadToken;
    return;
  }
  loadHistorical();
}
async function refreshSosHistory(){try{const a=await (await fetch('/api/sos-history')).json();sosBadge.textContent=a.map(x=>`event=${x.event} seq=${x.seq} peer=${x.peer}`).join(' | ')}catch(e){}}
async function refreshFiles(){try{f.textContent=await j('/api/files?dir='+encodeURIComponent(fileDir.value))}catch(e){toast('File list failed')}}
async function refresh(){
  const raw=await j('/api/status');s.textContent=raw;await refreshFiles();
  try{const x=JSON.parse(raw);
     const ra=x.runtimeAdvanced||{};
     deepSleepEnabled.checked=!!ra.deepSleepEnabled;
     deepSleepIdleSec.value=Math.round(ra.deepSleepIdleMs/1000);
     deepSleepWakeGraceMs.value=ra.deepSleepWakeGraceMs;
     criticalShutdownDelayMs.value=ra.criticalShutdownDelayMs;
     batteryLowThreshold.value=ra.batteryLowThreshold;
     batteryCriticalThreshold.value=ra.batteryCriticalThreshold;
     mqttEnabled.checked=!!ra.mqttEnabled;
     wakePeriodSec.value=String(ra.wakePeriodSec);
     staSsid.value=ra.staSsid;
     staPassword.value='';
     mqttHost.value=ra.mqttHost;
     mqttPort.value=ra.mqttPort;
     mqttTls.checked=!!ra.mqttTlsRequired;
     mqttRetryMin.value=ra.mqttReconnectMinMs;
     mqttRetryMax.value=ra.mqttReconnectMaxMs;
     mqttTelemetry.value=ra.mqttTelemetryPeriodMs;
     mqttHealth.value=ra.mqttHealthPeriodMs;
     mqttRetainTelemetry.checked=!!ra.mqttRetainTelemetry;
     mqttRetainAvailability.checked=!!ra.mqttRetainAvailability;
     estServerUrl.value=ra.estServerUrl;
     estLabel.value=ra.estLabel;
     certRenewalThresholdDays.value=ra.certRenewalThresholdDays;
     certCheckPeriodMs.value=ra.certCheckPeriodMs;
     estAuthMode.value=ra.estAuthMode;
     certLifecycleEnabled.checked=!!ra.certLifecycleEnabled;
     vox.checked=!!ra.voxEnabled;
     voxThreshold.value=ra.voxThreshold;
     voxHang.value=ra.voxHangMs;
     aec.checked=!!ra.aecEnabled;
     usbmon.checked=!!ra.usbMonitor;
     usbtransport.checked=!!ra.usbPlaybackTransport;
     loop.checked=!!ra.audioLoopback;
     adr.checked=!!ra.loraAdrEnabled;
     hop.checked=!!ra.loraHopEnabled;
     hopProfile.value=ra.loraHopChannelProfile;
     rangeMode.checked=!!ra.loraRangeTestMode;
     bleEnabled.checked=!!ra.sensorReaderEnabled;
     bleScanInterval.value=ra.sensorScanIntervalMs;
     bleScanWindow.value=ra.sensorScanWindowMs;
     bleScanDuration.value=ra.sensorScanDurationMs;
     bleConnectTimeout.value=ra.sensorConnectTimeoutMs;
     bleEviction.value=ra.sensorNodeEvictionMs;
     bleMaxNodes.value=ra.sensorMaxNodes;
     bleEncryption.checked=!!ra.sensorRequireEncryption;
     blePairing.checked=!!ra.blePairingEnabled;
     bleFailureThreshold.value=ra.blePairingFailureThreshold;
     bleBlockMs.value=ra.blePairingBlockMs;
     bleKeepAwake.checked=!!ra.sensorKeepAwake;
     webSessionTimeout.value=ra.webSessionTimeoutMs;
     webRateLimit.value=ra.webAuthRateLimitMs;
     csrfPolicy.value=ra.csrfPolicy;
     blePairingPolicy.value=ra.blePairingPolicy;
     ecdhPolicy.value=ra.ecdhRekeyPolicy;
     replayWindow.value=ra.replayWindowBits;
     document.getElementById('unreadBadge').textContent=(x.messageUnread||0)+' unread';document.getElementById('sosBadge').textContent=x.sosEscalated?'SOS ESCALATED':(x.sos?'SOS ACTIVE':'');document.body.classList.toggle('battery-low',!!x.battery?.low);document.body.classList.toggle('battery-critical',!!x.battery?.critical);
    document.getElementById('diagnostics').textContent =
      `Antenna OK: ${x.diagnostics?.antennaOk ? 'YES':'NO'} | TX RSSI: ${x.diagnostics?.txRssi} dBm | Baseline: ${x.diagnostics?.antennaBaselineRssi} dBm | CPU: ${x.cpuTempC} C | Battery calibration drift: ${x.batteryCalibrationDrift ? 'YES':'NO'}`;
  }catch(e){}
}
async function ptt(v){await j('/api/ptt?on='+v,{method:'POST'});refresh()}
async function sos(v){await j('/api/sos?on='+v,{method:'POST'});refresh()}
async function rec(v){await j('/api/record?on='+v,{method:'POST'});refresh()}
async function setAudioSource(){
  const r=await j('/api/audio-source',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({source:audsrc.value})});
  if(r!=='OK') alert(r);
  refresh();
}
async function setUsbMonitor(){
  const r=await j('/api/audio-monitor',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({on:usbmon.checked?'1':'0'})});
  if(r!=='OK') alert(r);
  refresh();
}
async function setLoopback(){
  const r=await j('/api/audio-loopback',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({on:loop.checked?'1':'0'})});
  if(r!=='OK') alert(r);
  refresh();
}
async function setAec(){
  const r=await j('/api/audio-aec',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({on:aec.checked?'1':'0'})});
  if(r!=='OK') alert(r);
  refresh();
}
async function setUsbTransport(){
  const r=await j('/api/usb-transport',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({on:usbtransport.checked?'1':'0'})});
  if(r!=='OK') alert(r);
  refresh();
}
async function tone(f,d){
  const r=await j('/api/audio-tone',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({freq:String(f),ms:String(d)})});
  if(r!=='OK') alert(r);
}
async function send(){await j('/api/message',{method:'POST',headers:{'Content-Type':'text/plain'},body:msg.value});refresh()}
async function uploadFile(){
 const file=upfile.files[0]; if(!file){alert('Choose a WAV file');return}
 const fd=new FormData(); fd.append('file',file,file.name);
 const r=await fetch('/api/upload',{method:'POST',headers:{'X-CSRF-Token':CSRF_TOKEN},body:fd}); alert(await r.text()); refresh()
}
async function renameFile(){
 const r=await j('/api/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
 body:new URLSearchParams({from:renameFrom.value,to:renameTo.value})}); alert(r); refresh()
}
async function calBattery(){
 const r=await j('/api/battery-calibrate',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
 body:new URLSearchParams({voltage:batActual.value})}); alert(r); refresh()
}
async function factoryReset(){
 if(!confirm('Factory reset will erase NVS and SD data. Continue?'))return;
 const r=await j('/api/factory-reset?confirm=RESET',{method:'POST'});
 alert(r);
}
async function saveCfg(){
  const q=new URLSearchParams({freq:freq.value,bw:bw.value,sf:sf.value,cr:cr.value,
    power:pwr.value,sync:sw.value,callsign:cs.value,volume:vol.value,batcal:bat.value,
    audio_source:audsrc.value,mqtt_enabled:mqttEnabled.checked?'1':'0',
    wake_period_sec:wakePeriodSec.value,
    sta_ssid:staSsid.value,
    deep_sleep_enabled:deepSleepEnabled.checked?'1':'0',
    deep_sleep_idle_sec:deepSleepIdleSec.value,
    deep_sleep_wake_grace_ms:deepSleepWakeGraceMs.value,
    critical_shutdown_delay_ms:criticalShutdownDelayMs.value,
    battery_low_threshold:batteryLowThreshold.value,
    battery_critical_threshold:batteryCriticalThreshold.value,
    classd_enabled:classDEnabled.checked?'1':'0',
    classd_boost:classDBoost.value,
    mqtt_host:mqttHost.value,mqtt_port:mqttPort.value,mqtt_tls:mqttTls.checked?'1':'0',
    mqtt_reconnect_min_ms:mqttRetryMin.value,mqtt_reconnect_max_ms:mqttRetryMax.value,
    mqtt_telemetry_period_ms:mqttTelemetry.value,mqtt_health_period_ms:mqttHealth.value,
    mqtt_retain_telemetry:mqttRetainTelemetry.checked?'1':'0',
    mqtt_retain_availability:mqttRetainAvailability.checked?'1':'0',
    est_server_url:estServerUrl.value,
    est_label:estLabel.value,
    cert_renewal_threshold_days:certRenewalThresholdDays.value,
    cert_check_period_ms:certCheckPeriodMs.value,
    est_auth_mode:estAuthMode.value,
    cert_lifecycle_enabled:certLifecycleEnabled.checked?'1':'0',
    vox_enabled:vox.checked?'1':'0',vox_threshold:voxThreshold.value,vox_hang_ms:voxHang.value,
    aec_enabled:aec.checked?'1':'0',usb_monitor:usbmon.checked?'1':'0',
    usb_transport:usbtransport.checked?'1':'0',loopback:loop.checked?'1':'0',
    adr_enabled:adr.checked?'1':'0',hop_enabled:hop.checked?'1':'0',hop_profile:hopProfile.value,
    range_test_mode:rangeMode.checked?'1':'0',
    ble_enabled:bleEnabled.checked?'1':'0',ble_scan_interval_ms:bleScanInterval.value,
    ble_scan_window_ms:bleScanWindow.value,ble_scan_duration_ms:bleScanDuration.value,
    ble_connect_timeout_ms:bleConnectTimeout.value,ble_eviction_ms:bleEviction.value,
    ble_max_nodes:bleMaxNodes.value,ble_encryption:bleEncryption.checked?'1':'0',
    ble_pairing:blePairing.checked?'1':'0',ble_failure_threshold:bleFailureThreshold.value,
    ble_block_ms:bleBlockMs.value,ble_keep_awake:bleKeepAwake.checked?'1':'0',
    web_session_timeout_ms:webSessionTimeout.value,web_auth_rate_limit_ms:webRateLimit.value,
    csrf_policy:csrfPolicy.value,ble_pairing_policy:blePairingPolicy.value,
    ecdh_rekey_policy:ecdhPolicy.value,replay_window_bits:replayWindow.value});
  if(key.value)q.set('lora_key',key.value);
  if(staPassword.value)q.set('sta_password',staPassword.value);
  q.set('sta_ssid',staSsid.value);
  if(aps.value)q.set('ap_password',aps.value);
  if(wp.value)q.set('web_password',wp.value);
  if(estUsername.value) q.set('est_username',estUsername.value);
  if(estPassword.value) q.set('est_password',estPassword.value);
  if(estBootstrapToken.value) q.set('est_bootstrap_token',estBootstrapToken.value);
  alert(await j('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q}));refresh()
}
async function refreshCertStatus(){
 try{const a=await (await fetch('/api/mqtt/cert-status')).json();certStatus.textContent=JSON.stringify(a,null,2)}catch(e){certStatus.textContent='Certificate status unavailable'}
}
async function renewCert(){const r=await j('/api/mqtt/cert-renew',{method:'POST'});toast(r);await refreshCertStatus()}
async function fetchCertCa(){const r=await j('/api/mqtt/cert-cacerts',{method:'POST'});toast(r)}
async function refreshCertHistory(){try{const a=await (await fetch('/api/mqtt/cert-history')).json();certHistory.textContent=JSON.stringify(a,null,2)}catch(e){}}
setInterval(refreshCertStatus,30000);refreshCertStatus();refreshCertHistory();
async function play(){await j('/api/play?path='+encodeURIComponent(file.value),{method:'POST'});refresh()}
async function stopPlay(){await j('/api/stop',{method:'POST'});refresh()}
async function pausePlay(v){await j('/api/pause?on='+v,{method:'POST'});refresh()}
async function seekPlay(){await j('/api/seek?ms='+encodeURIComponent(seekms.value),{method:'POST'});refresh()}
async function queue(){await j('/api/queue?path='+encodeURIComponent(file.value),{method:'POST'});refresh()}
async function clearQueue(){await j('/api/queue-clear',{method:'POST'});refresh()}
async function recordPause(v){await j('/api/record-pause?on='+v,{method:'POST'});refresh()}
async function recordSplit(){await j('/api/record-split',{method:'POST'});refresh()}
async function setVox(){await j('/api/vox?on='+(vox.checked?'1':'0')+'&threshold='+encodeURIComponent(voxThreshold.value)+'&hang='+encodeURIComponent(voxHang.value),{method:'POST'});refresh()}
async function setRecordQuality(){await j('/api/record/quality?level='+encodeURIComponent(recordQuality.value),{method:'POST'});refresh()}
async function syncSource(){
  try {
    const x=await (await fetch('/api/status')).json();
    if(x.audioSource!==undefined)audsrc.value=String(x.audioSource);
    if(x.usbMonitor!==undefined)usbmon.checked=!!x.usbMonitor;
    if(x.audioLoopback!==undefined)loop.checked=!!x.audioLoopback;
    if(x.usbPlaybackTransport!==undefined)usbtransport.checked=!!x.usbPlaybackTransport;
    if(x.runtimeAdvanced){
      mqttEnabled.checked=!!x.runtimeAdvanced.mqttEnabled;
      wakePeriodSec.value=String(x.runtimeAdvanced.wakePeriodSec);
      deepSleepEnabled.checked=!!x.runtimeAdvanced.deepSleepEnabled;
      deepSleepIdleSec.value=String(Math.round(x.runtimeAdvanced.deepSleepIdleMs/1000));
      deepSleepWakeGraceMs.value=String(x.runtimeAdvanced.deepSleepWakeGraceMs);
      criticalShutdownDelayMs.value=String(x.runtimeAdvanced.criticalShutdownDelayMs);
      batteryLowThreshold.value=String(x.runtimeAdvanced.batteryLowThreshold);
      batteryCriticalThreshold.value=String(x.runtimeAdvanced.batteryCriticalThreshold);
      classDEnabled.disabled=!x.runtimeAdvanced.classDHardwareEnabled;
      classDEnabled.checked=!!x.runtimeAdvanced.classDEnabled;
      classDBoost.disabled=!x.runtimeAdvanced.classDHardwareEnabled;
      classDBoost.value=String(x.runtimeAdvanced.classDBoostLevel);
    }
  } catch(e){}
}
async function scanStart(mode){
  const r=await j('/api/scan/start?mode='+mode+'&dwell=100',{method:'POST'});
  document.getElementById('scanmsg').textContent=r; refreshScan();
}
async function scanStop(){
  const r=await j('/api/scan/stop',{method:'POST'});
  document.getElementById('scanmsg').textContent=r; refreshScan();
}
async function hopSuggest(){
  const r=await j('/api/hop/suggest',{method:'POST'});
  document.getElementById('scanmsg').textContent='Suggest: '+r; refreshHop();
}
async function hopEnable(on){
  const r=await j('/api/hop/enable?on='+on,{method:'POST'});
  document.getElementById('scanmsg').textContent=r; refreshHop();
}
async function hopSetChannels(){
  const r=await j('/api/hop/set-channels?list='+encodeURIComponent(hopList.value),{method:'POST'});
  document.getElementById('scanmsg').textContent=r; refreshHop();
}
async function rangeTest(on){
  const r=await j('/api/range-test?on='+on,{method:'POST'});
  toast(r);refresh();
}
async function refreshScan(){
  try{
    const arr=await (await fetch('/api/scan/results')).json();
    let html='';
    for(const r of arr){
      const w=Math.min(100,r.occupancy|0);
      html+='<tr><td>'+r.freq.toFixed(3)+'</td><td>'+r.rssiAvg+
            '</td><td>'+r.rssiPeak+'</td><td>'+r.snr+
            '</td><td>'+r.occupancy+'%</td><td>'+r.preamble+
            '</td><td><div style="width:'+w+
            '%;height:10px"></div></td></tr>';
    }
    document.getElementById('scantbody').innerHTML=html;
  }catch(e){}
}
async function refreshHop(){
  try{
    const h=await (await fetch('/api/hop/status')).json();
    document.getElementById('hopsummary').textContent=
      'Hop enabled='+h.enabled+' count='+h.count+
      ' dwell='+h.dwellMs+'ms channels=['+h.channels.join(',')+']';
  }catch(e){}
}
async function refreshLw(){
  try{
    const x=await (await fetch('/api/lorawan/status')).json();
    lwStatus.textContent=JSON.stringify(x,null,2);
    if(x.region!==undefined)lwRegion.value=String(x.region);
    if(x.mode!==undefined)lwMode.value=String(x.mode);
  }catch(e){}
}
async function saveLw(){
  const q=new URLSearchParams({enabled:'1',region:lwRegion.value,mode:lwMode.value,
    deveui:lwDevEui.value,joineui:lwJoinEui.value,appkey:lwAppKey.value,
    devaddr:lwDevAddr.value,nwkskey:lwNwkSKey.value,appskey:lwAppSKey.value,
    fport:lwFPort.value,period:lwPeriod.value});
  alert(await j('/api/lorawan/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q}));
  refreshLw();
}
async function lwConnect(){toast(await j('/api/lorawan/connect',{method:'POST'}));refreshLw()}
async function lwDisconnect(){toast(await j('/api/lorawan/disconnect',{method:'POST'}));refreshLw()}
async function lwUplink(){
  const v=lwPayload.value||'';
  const q=v.startsWith('hex:')?new URLSearchParams({hex:v.substring(4)}):new URLSearchParams({text:v});
  toast(await j('/api/lorawan/uplink',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q}));
  refreshLw();
}
setInterval(refresh,1000);setInterval(()=>{if(document.getElementById('trackPanel').style.display==='block')refreshMap()},2000);syncSource();refresh();refreshMessages();refreshRadioHistory();refreshSosHistory()
setInterval(refreshLw,2000);refreshLw();setInterval(refreshScan,3000);setInterval(refreshHop,3000);setInterval(refreshMessages,2000);setInterval(refreshRadioHistory,3000);refreshScan();refreshHop()
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// Free helpers (lanjutan Bagian 1)
// ---------------------------------------------------------------------------

static String configBackupPlaintext() {
  RuntimeConfig c;
  if (!configSnapshot(c)) return String();
  String p;
  p.reserve(512);
  p += "freq=" + String(c.loraFreqMHz, 6) + "\n";
  p += "bw=" + String(c.loraBwKHz, 6) + "\n";
  p += "sf=" + String(c.loraSf) + "\n";
  p += "cr=" + String(c.loraCr) + "\n";
  p += "sync=" + String(c.loraSyncWord) + "\n";
  p += "power=" + String(c.loraPowerDbm) + "\n";
  p += "volume=" + String(c.volume) + "\n";
  p += "audsrc=" + String(c.audioRecordSource) + "\n";
  p += "recqual=" + String(c.audioRecordQuality) + "\n";
  p += "batcal=" + String(c.batteryCalibration, 6) + "\n";
  p += "callsign=" + c.callsign + "\n";
  p += "lorakey=" + c.loraKeyHex + "\n";
  p += "apssid=" + c.apSsid + "\n";
  p += "apppass=" + c.apPassword + "\n";
  p += "stassid=" + c.staSsid + "\n";
  p += "stapass=" + c.staPassword + "\n";
  p += "webuser=" + c.webUser + "\n";
  p += "websalt=" + c.webPasswordSaltHex + "\n";
  p += "webph=" + c.webPasswordHashHex + "\n";
  p += "mqtt_en=" + String(c.mqttEnabled ? 1 : 0) + "\n";
  p += "wake_sec=" + String(c.wakePeriodSec) + "\n";
  p += "sleep_en=" + String(c.deepSleepEnabled ? 1 : 0) + "\n";
  p += "sleep_idle=" + String(c.deepSleepIdleMs) + "\n";
  p += "wake_grace=" + String(c.deepSleepWakeGraceMs) + "\n";
  p += "bat_crit_delay=" + String(c.criticalShutdownDelayMs) + "\n";
  p += "bat_low=" + String(c.batteryLowThreshold, 3) + "\n";
  p += "bat_critical=" + String(c.batteryCriticalThreshold, 3) + "\n";
  p += "classd_en=" + String(c.classDEnabled ? 1 : 0) + "\n";
  p += "classd_boost=" + String(c.classDBoostLevel) + "\n";
  return p;
}

static bool backupKey(uint8_t key[32]) {
  RuntimeConfig config;
  if (!key || !configSnapshot(config) || config.webPasswordHashHex.length() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const char a = config.webPasswordHashHex[i * 2];
    const char b = config.webPasswordHashHex[i * 2 + 1];
    auto n = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = n(a), lo = n(b);
    if (hi < 0 || lo < 0) return false;
    key[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

static String encryptConfigBackup() {
  uint8_t key[32] = {};
  if (!backupKey(key)) return String();
  uint8_t iv[16] = {};
  for (size_t i = 0; i < sizeof(iv); i += 4) {
    const uint32_t r = esp_random();
    memcpy(iv + i, &r, min<size_t>(4, sizeof(iv) - i));
  }
  const String plain = configBackupPlaintext();
  String cipherHex;
  cipherHex.reserve(plain.length() * 2);
  uint8_t* cipher = static_cast<uint8_t*>(malloc(plain.length()));
  if (!cipher) return String();
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t ncOff = 0;
  uint8_t stream[16] = {};
  uint8_t ctr[16] = {};
  memcpy(ctr, iv, sizeof(iv));
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
                  mbedtls_aes_crypt_ctr(&aes, plain.length(), &ncOff, ctr, stream,
                                        reinterpret_cast<const unsigned char*>(plain.c_str()), cipher) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) { free(cipher); return String(); }
  cipherHex = hexEncodeUi(cipher, plain.length());
  free(cipher);

  String envelope = "FRB1|" + hexEncodeUi(iv, sizeof(iv)) + "|" + cipherHex;
  uint8_t mac[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
                             reinterpret_cast<const uint8_t*>(envelope.c_str()),
                             envelope.length(), mac) != 0)
    return String();
  return envelope + "|" + hexEncodeUi(mac, sizeof(mac));
}

static bool decryptConfigBackup(const String& envelope, String& plain) {
  uint8_t key[32] = {};
  if (!backupKey(key)) return false;
  const int p1 = envelope.indexOf('|');
  const int p2 = p1 >= 0 ? envelope.indexOf('|', p1 + 1) : -1;
  const int p3 = p2 >= 0 ? envelope.indexOf('|', p2 + 1) : -1;
  if (p1 != 4 || p2 <= p1 || p3 <= p2) return false;
  const String tag = envelope.substring(p3 + 1);
  uint8_t iv[16] = {}, mac[32] = {};
  if (!hexDecodeUi(envelope.substring(p1 + 1, p2), iv, sizeof(iv)) ||
      !hexDecodeUi(tag, mac, sizeof(mac)))
    return false;
  const String signedPart = envelope.substring(0, p3);
  uint8_t expected[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
                             reinterpret_cast<const uint8_t*>(signedPart.c_str()),
                             signedPart.length(), expected) != 0)
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(mac); ++i) diff |= mac[i] ^ expected[i];
  if (diff) return false;
  const String cipherHex = envelope.substring(p2 + 1, p3);
  if (cipherHex.length() == 0 || (cipherHex.length() & 1U)) return false;
  const size_t len = cipherHex.length() / 2;
  uint8_t* cipher = static_cast<uint8_t*>(malloc(len));
  uint8_t* out = static_cast<uint8_t*>(malloc(len + 1));
  if (!cipher || !out || !hexDecodeUi(cipherHex, cipher, len)) {
    free(cipher); free(out); return false;
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t ncOff = 0;
  uint8_t stream[16] = {}, ctr[16] = {};
  memcpy(ctr, iv, sizeof(iv));
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
                  mbedtls_aes_crypt_ctr(&aes, len, &ncOff, ctr, stream,
                                        cipher, out) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) { free(cipher); free(out); return false; }
  out[len] = '\0';
  plain = String(reinterpret_cast<char*>(out));
  free(cipher); free(out);
  return true;
}

static bool parseBackupLine(const String& line, String& key, String& value) {
  const int eq = line.indexOf('=');
  if (eq <= 0) return false;
  key = line.substring(0, eq);
  value = line.substring(eq + 1);
  return true;
}

static bool isValidUploadedWav(const String& path) {
  if (path.length() < 4 ||
      !path.substring(path.length() - 4).equalsIgnoreCase(".WAV"))
    return false;
  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) return false;
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory() || f.size() < 44) {
    if (f) f.close();
    return false;
  }
  uint8_t header[12] = {};
  const size_t got = f.read(header, sizeof(header));
  f.close();
  return got == sizeof(header) &&
         memcmp(header, "RIFF", 4) == 0 &&
         memcmp(header + 8, "WAVE", 4) == 0;
}

static bool eraseStorageTree(const char* path) {
  if (!path || !*path) return false;
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  bool ok = true;
  for (;;) {
    File entry = dir.openNextFile();
    if (!entry) break;
    String child = entry.name();
    const bool isDir = entry.isDirectory();
    entry.close();
    if (!child.startsWith("/")) {
      child = String(path) + "/" + child;
    }

    const bool childOk = isDir
        ? eraseStorageTree(child.c_str())
        : SD.remove(child);
    if (!childOk) ok = false;
  }
  dir.close();
  return ok;
}

// ---------------------------------------------------------------------------
// External dependencies (unchanged from pre-migration WebUi.cpp)
// ---------------------------------------------------------------------------
extern StorageManager storage;
extern LoRaManager lora;
extern LoRaWANManager lorawan;
extern AudioManager audio;
extern MqttClientManager mqtt;
extern BleSensorReader bleSensorReader;
extern void fieldRadioRequestDeepSleep();
static SensorReader::SensorNodeSnapshot gSensorSnapshots[SensorRegistry::MAX_SUPPORTED_NODES]{};

// ---------------------------------------------------------------------------
// Sensor JSON helpers (unchanged from pre-migration WebUi.cpp)
// ---------------------------------------------------------------------------
static String sensorAddressJson(const SensorProtocol::BleAddress& address) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(18);
  for (int i = 5; i >= 0; --i) {
    if (i != 5) out += ':';
    out += hex[address.bytes[i] >> 4];
    out += hex[address.bytes[i] & 0x0F];
  }
  return out;
}

static String sensorNodeJson(size_t index, const SensorRegistry::Node& node, bool includeValues) {
  String j = "{\"id\":" + String(index) + ",\"address\":\"" + sensorAddressJson(node.address) +
             "\",\"name\":\"" + jsonEscape(String(node.name)) + "\",\"rssi\":" + String(node.rssi) +
             ",\"connected\":" + String(node.connected ? "true" : "false") +
             ",\"lastSeenMs\":" + String(node.lastSeenMs) + ",\"sensorCount\":" + String(node.sensorCount);
  if (node.hasRPA) j += ",\"rpa\":\"" + sensorAddressJson(node.lastRPA) + "\"";
  if (includeValues) {
    j += ",\"sensors\":[";
    for (size_t i = 0; i < node.sensorCount; ++i) {
      if (i) j += ',';
      const auto& d = node.descriptors[i];
      const auto& v = node.values[i];
      j += "{\"id\":" + String(d.id) + ",\"name\":\"" + jsonEscape(String(d.name)) +
           "\",\"unit\":\"" + jsonEscape(String(d.unit)) + "\",\"valueValid\":" +
           String(node.valueValid[i] ? "true" : "false") + ",\"value\":" +
           (node.valueValid[i] ? String(v.value, 6) : String("null")) +
           ",\"timestamp\":" + (node.valueValid[i] ? String(v.timestamp) : String("null")) +
           ",\"quality\":" + String(node.valueValid[i] ? v.quality : 0) + "}";
    }
    j += ']';
  }
  return j + '}';
}

// ---------------------------------------------------------------------------
// Config audit (unchanged)
// ---------------------------------------------------------------------------
static void auditConfigChange(const RuntimeConfig& previous,
                              const RuntimeConfig& current,
                              const char* reason) {
  if (!storage.ready()) return;
  SpiLock spiLock(pdMS_TO_TICKS(50));
  if (!spiLock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  const char* path = "/LOG/CONFIG-AUDIT.LOG";
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (f.size() >= Config::CONFIG_AUDIT_LOG_ROTATE_BYTES) {
    f.close();
    const char* old = "/LOG/CONFIG-AUDIT.1.LOG";
    if (SD.exists(old)) SD.remove(old);
    if (SD.exists(path)) SD.rename(path, old);
    f = SD.open(path, FILE_APPEND);
  }
  if (!f) return;
  StateLock lock(gState);
  const uint64_t ts = lock.ok() && gState.gps.timeValid ? gState.gps.utcEpoch : millis();
  f.printf("%llu,%s,freq=%.3f,bw=%.1f,sf=%u,pwr=%d,volume=%u,audio=%u,reason=%s\n",
           static_cast<unsigned long long>(ts), current.callsign.c_str(),
           current.loraFreqMHz, current.loraBwKHz,
           current.loraSf, current.loraPowerDbm, current.volume,
           current.audioRecordSource, reason ? reason : "web");
  f.close();
}

// ---------------------------------------------------------------------------
// begin() — register all endpoints and start TLS server
// ---------------------------------------------------------------------------
void WebUi::begin() {
#if !WEB_TLS_CERT_CONFIGURED
  // Never fall back to plaintext HTTP when certificate provisioning is absent.
  // The WebUI remains disabled until a device-specific certificate/key pair is
  // provisioned through secrets/.
  Serial.println("WebUI HTTPS disabled: TLS certificate provisioning missing");
  return;
#endif

  // Every endpoint runs through the native HttpdServer. auth() and csrfValid()
  // are per-request; the lambdas capture `this` so no per-endpoint state has
  // to be stored in the handler table.
  auto reg = [this](const char* uri, httpd_method_t method,
                    std::function<void(HttpdRequest&, HttpdResponse&)> fn) {
    if (!server_.on(uri, method, std::move(fn))) {
      Serial.printf("WebUI: failed to register %s\n", uri);
    }
  };

  // ---- Root & meta ----
  reg("/", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRoot(req, res);
  });
  reg("/api/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStatus(req, res);
  });
  reg("/api/v1/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStatus(req, res);
  });
  reg("/api/version", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleApiVersion(req, res);
  });
  reg("/api/v1/version", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleApiVersion(req, res);
  });
  reg("/api/v1/csrf", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    res.sendJson(200, "{\"token\":\"" + csrfTokenHexForActiveSession() + "\"}");
  });

  // ---- LoRaWAN ----
  reg("/api/lorawan/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANStatus(req, res);
  });
  reg("/api/lorawan/connect", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANConnect(req, res);
  });
  reg("/api/lorawan/disconnect", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANDisconnect(req, res);
  });
  reg("/api/lorawan/config", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANConfig(req, res);
  });
  reg("/api/lorawan/uplink", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANUplink(req, res);
  });

  // ---- Files ----
  reg("/api/files", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleFiles(req, res);
  });
  reg("/api/download", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDownload(req, res);
  });
  reg("/api/upload", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleUpload(req, res);
  });
  reg("/api/rename", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRename(req, res);
  });

  // ---- Messages ----
  reg("/api/message", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessage(req, res);
  });
  reg("/api/messages", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessages(req, res);
  });
  reg("/api/messages/clear", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageClear(req, res);
  });
  reg("/api/messages/read", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageRead(req, res);
  });
  reg("/api/messages/reply", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageReply(req, res);
  });
  reg("/api/messages/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageExport(req, res);
  });
  reg("/api/messages/persist", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessagePersist(req, res);
  });
  reg("/api/message/schedule", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageSchedule(req, res);
  });
  reg("/api/message/schedule", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageScheduleList(req, res);
  });
  reg("/api/message/schedule", HTTP_DELETE, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageScheduleDelete(req, res);
  });
  reg("/api/record/schedule", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordSchedule(req, res);
  });
  reg("/api/record/schedule", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordScheduleGet(req, res);
  });

  // ---- SOS ----
  reg("/api/sos/format", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSosFormat(req, res);
  });
  reg("/api/sos-history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSosHistory(req, res);
  });
  reg("/api/sos", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSos(req, res);
  });
  reg("/api/sos-status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSosStatus(req, res);
  });

  // ---- Self test ----
  reg("/api/selftest", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSelfTest(req, res);
  });
  reg("/api/selftest/result", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSelfTestResult(req, res);
  });

  // ---- UI prefs ----
  reg("/api/lang", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLang(req, res);
  });
  reg("/api/theme", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTheme(req, res);
  });

  // ---- Diagnostics ----
  reg("/api/neighbors", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleNeighbors(req, res);
  });
  reg("/api/routes", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRoutes(req, res);
  });
  reg("/api/dedup/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDedupStats(req, res);
  });
  reg("/api/forward/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleForwardStats(req, res);
  });
  reg("/api/ecdh/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) res.sendJson(200, lora.ecdhStatusJson());
  });
  reg("/api/auth/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleAuthStats(req, res);
  });
  reg("/api/nvs", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleNvs(req, res);
  });
  reg("/api/config/migrate", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigMigrate(req, res);
  });
  reg("/api/diag/full", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDiagFull(req, res);
  });
  reg("/api/health-log", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHealthLog(req, res);
  });
  reg("/api/lora-log", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoraLog(req, res);
  });

  // ---- Capture / ADR / HOP ----
  reg("/api/capture/start", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleCaptureStart(req, res);
  });
  reg("/api/capture/stop", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleCaptureStop(req, res);
  });
  reg("/api/capture/dump", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleCaptureDump(req, res);
  });
  reg("/api/adr", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleAdr(req, res);
  });
  reg("/api/hop/sync", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopSync(req, res);
  });
  reg("/api/hop/suggest", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopSuggest(req, res);
  });
  reg("/api/hop/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopStatus(req, res);
  });
  reg("/api/hop/enable", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopEnable(req, res);
  });
  reg("/api/hop/set-channels", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopSetChannels(req, res);
  });

  // ---- Scanner / range test ----
  reg("/api/scan/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanStatus(req, res);
  });
  reg("/api/scan/start", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanStart(req, res);
  });
  reg("/api/scan/stop", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanStop(req, res);
  });
  reg("/api/scan/results", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanResults(req, res);
  });
  reg("/api/range-test", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRangeTest(req, res);
  });
  reg("/api/range-test/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRangeTestStatus(req, res);
  });
  reg("/api/range-test/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRangeTestStatus(req, res);
  });

  // ---- Radio / battery / storage ----
  reg("/api/radio/history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRadioHistory(req, res);
  });
  reg("/api/radio/tune", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRadioTune(req, res);
  });
  reg("/api/radio/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRadioStats(req, res);
  });
  reg("/api/rf/detector", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRfDetector(req, res);
  });
  reg("/api/battery/history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBatteryHistory(req, res);
  });
  reg("/api/battery-calibrate", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBatteryCalibrate(req, res);
  });
  reg("/api/storage/info", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStorageInfo(req, res);
  });
  reg("/api/storage/checksum", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleChecksum(req, res);
  });
  reg("/api/storage/checksum-sha256", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleChecksumSha256(req, res);
  });
  reg("/api/log/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLogExport(req, res);
  });

  // ---- Audio ----
  reg("/api/ptt", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handlePtt(req, res);
  });
  reg("/api/record", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecord(req, res);
  });
  reg("/api/record/quality", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordQuality(req, res);
  });
  reg("/api/play", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handlePlay(req, res);
  });
  reg("/api/stop", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStop(req, res);
  });
  reg("/api/pause", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handlePause(req, res);
  });
  reg("/api/seek", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSeek(req, res);
  });
  reg("/api/queue", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleQueue(req, res);
  });
  reg("/api/queue-clear", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleQueueClear(req, res);
  });
  reg("/api/record-pause", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordPause(req, res);
  });
  reg("/api/record-split", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordSplit(req, res);
  });
  reg("/api/vox", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleVox(req, res);
  });
  reg("/api/vad", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleVad(req, res);
  });
  reg("/api/usb-transport", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleUsbTransport(req, res);
  });
  reg("/api/volume", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleVolume(req, res);
  });
  reg("/api/delete", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDelete(req, res);
  });
  reg("/api/audio-source", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleAudioSource(req, res);
  });
  reg("/api/audio-monitor", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String raw = req.arg("on");
    if (raw != "0" && raw != "1") { res.sendText(400, "invalid monitor"); return; }
    const bool ok = audio.setUsbMonitor(raw == "1");
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });
  reg("/api/audio-loopback", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String raw = req.arg("on");
    if (raw != "0" && raw != "1") { res.sendText(400, "invalid loopback"); return; }
    const bool ok = audio.setLoopback(raw == "1");
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });
  reg("/api/audio-aec", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String raw = req.arg("on");
    if (raw != "0" && raw != "1") { res.sendText(400, "invalid aec"); return; }
    const bool ok = audio.setAec(raw == "1");
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });
  reg("/api/audio-tone", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String rf = req.arg("freq");
    const String rm = req.arg("ms");
    if (rf.isEmpty() || rm.isEmpty() || rf.length() > 5 || rm.length() > 4) {
      res.sendText(400, "invalid tone"); return;
    }
    const int f = rf.toInt();
    const int ms = rm.toInt();
    const bool ok = f >= 1 && f <= 10000 && ms >= 1 &&
                    ms <= static_cast<int>(Config::AUDIO_TONE_MAX_MS) &&
                    audio.playTone(static_cast<uint16_t>(f), static_cast<uint16_t>(ms));
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });

  // ---- Track / GPS ----
  reg("/api/track", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrack(req, res);
  });
  reg("/api/track/points", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrackPoints(req, res);
  });
  reg("/api/track/simplified", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrackSimplified(req, res);
  });
  reg("/api/track/download", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrackDownload(req, res);
  });

  // ---- BLE sensors ----
  reg("/api/sensors/nodes", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    if (req.hasArg("id")) handleSensorNodeDetail(req, res);
    else handleSensorNodes(req, res);
  });
  reg("/api/sensors/live", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorLive(req, res);
  });
  reg("/api/sensors/forget", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorForget(req, res);
  });
  reg("/api/sensors/refresh", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorRefresh(req, res);
  });
  reg("/api/sensors/queue-policy", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorQueuePolicy(req, res);
  });
  reg("/api/sensors/dedup-stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorDedupStats(req, res);
  });
  reg("/api/sensors/spool", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorSpool(req, res);
  });
  reg("/api/sensors/spool/clear", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorSpoolClear(req, res);
  });
  reg("/api/ble/passkey", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBlePasskeySet(req, res);
  });
  reg("/api/ble/passkey", HTTP_DELETE, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBlePasskeyDelete(req, res);
  });
  reg("/api/ble/passkey", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBlePasskeyList(req, res);
  });

  // ---- MQTT ----
  reg("/api/sensors/legacy-migration", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String value = req.arg("enabled");
    if (value != "true" && value != "false" && value != "1" && value != "0") {
      res.sendJson(400, "{\"ok\":false,\"error\":\"enabled must be true|false\"}");
      return;
    }
    if (!gConfigMutex || xSemaphoreTake(gConfigMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      res.sendJson(503, "{\"ok\":false,\"error\":\"config unavailable\"}");
      return;
    }
    gConfig.migrationWindowActive = (value == "true" || value == "1");
    gConfigGeneration.fetch_add(1, std::memory_order_release);
    xSemaphoreGive(gConfigMutex);
    res.sendJson(200, String("{\"ok\":true,\"migrationWindowActive\":") +
                      (gConfig.migrationWindowActive ? "true}" : "false}"));
  });
  reg("/api/mqtt/provision", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttProvision(req, res);
  });
  reg("/api/mqtt/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttStatus(req, res);
  });
  reg("/api/mqtt/cert-status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertStatus(req, res);
  });
  reg("/api/mqtt/cert-renew", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertRenew(req, res);
  });
  reg("/api/mqtt/credential-rotate", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const bool ok = certLifecycle.renewCertificate(true);
    res.sendJson(ok ? 200 : 503, ok
        ? "{\"ok\":true,\"status\":\"rotation completed\"}"
        : "{\"ok\":false,\"error\":\"credential rotation failed; inspect lifecycle status and audit log\"}");
  });
  reg("/api/mqtt/cert-history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertHistory(req, res);
  });
  reg("/api/mqtt/cert-cacerts", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertCaChain(req, res);
  });

  // ---- Config ----
  reg("/api/config", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfig(req, res);
  });
  reg("/api/config/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigExport(req, res);
  });
  reg("/api/config/backup", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigBackup(req, res);
  });
  reg("/api/config/restore", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigRestore(req, res);
  });
  reg("/api/factory-reset", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleFactoryReset(req, res);
  });

  // ---- Reboot / deep-sleep ----
  reg("/api/reboot", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleReboot(req, res);
  });
  reg("/api/deep-sleep", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) fieldRadioRequestDeepSleep();
  });

  // ---- Start TLS server ----
  if (!server_.begin(WEB_TLS_CERT_DER, WEB_TLS_CERT_DER_LEN,
                     WEB_TLS_KEY_DER, WEB_TLS_KEY_DER_LEN,
                     Config::WEB_PORT)) {
    Serial.println("WebUI: httpd_ssl_start failed; HTTPS service disabled");
    return;
  }
  Serial.printf("WebUI: HTTPS server ready on port %u (%u handlers)\n",
                static_cast<unsigned>(Config::WEB_PORT),
                static_cast<unsigned>(server_.registeredCount()));
}

// ---------------------------------------------------------------------------
// task()
// ---------------------------------------------------------------------------
// The native httpd server owns its own FreeRTOS task. WebUi::task() therefore
// only services the in-process record schedule state machine; it no longer
// drives HTTP request processing.
void WebUi::task() {
  if (recordScheduleActive_ && recordScheduleStart_) {
    bool recording = false;
    uint64_t now = 0;
    {
      StateLock lock(gState);
      if (lock.ok() && gState.gps.timeValid) {
        now = gState.gps.utcEpoch;
        recording = gState.recording;
      }
    }
    if (now >= recordScheduleStart_) {
      if (!recording) (void)audio.startRecording();
      if (now >= recordScheduleStart_ + recordScheduleDurationSec_) {
        (void)audio.stopRecording();
        recordScheduleActive_ = false;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Auth / session / CSRF helpers
// ---------------------------------------------------------------------------

bool WebUi::sameOrigin(HttpdRequest& req) {
  const String origin = req.header("Origin");
  if (origin.isEmpty() || !origin.startsWith("https://")) return false;

  const String host = req.header("Host");
  const String apOrigin = String("https://") + WiFi.softAPIP().toString();
  if (origin == apOrigin) return true;

  if (!host.isEmpty()) {
    String expected = String("https://") + host;
    if (expected.endsWith(":443"))
      expected.remove(expected.length() - 4);
    return origin == expected;
  }
  return false;
}

bool WebUi::rateLimit(HttpdRequest& req, HttpdResponse& res,
                      uint32_t& last, uint32_t interval) {
  struct RateSlot {
    uintptr_t endpoint = 0;
    String ip;
    uint32_t last = 0;
  };
  static RateSlot slots[32];
  const uint32_t now = millis();
  const String ip = req.remoteIp().toString();
  const uintptr_t endpoint = reinterpret_cast<uintptr_t>(&last);

  RateSlot* slot = nullptr;
  RateSlot* oldest = &slots[0];
  for (auto& candidate : slots) {
    if (candidate.endpoint == endpoint && candidate.ip == ip) {
      slot = &candidate;
      break;
    }
    if (candidate.last < oldest->last) oldest = &candidate;
  }
  if (!slot) {
    slot = oldest;
    slot->endpoint = endpoint;
    slot->ip = ip;
    slot->last = 0;
  }
  if (slot->last != 0 && now - slot->last < interval) {
    res.send429("rate limited");
    return false;
  }
  slot->last = now;
  last = now;
  return true;
}

int WebUi::findSessionSlot(const uint8_t token[32], uint32_t clientIp) const {
  if (!token) return -1;
  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    const SessionSlot& slot = sessions_[i];
    if (!slot.inUse || slot.clientIp != clientIp) continue;
    uint8_t msg[8] = {};
    WebUiSessionPolicy::makeSessionMessage(clientIp, slot.issuedMs, msg);
    uint8_t expected[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, slot.secret, sizeof(slot.secret),
                               msg, sizeof(msg), expected, sizeof(expected)) != 0)
      continue;
    uint8_t diff = 0;
    for (size_t j = 0; j < sizeof(expected); ++j)
      diff |= static_cast<uint8_t>(token[j] ^ expected[j]);
    if (diff == 0) return static_cast<int>(i);
  }
  return -1;
}

bool WebUi::sessionValid(HttpdRequest& req) {
  activeSessionSlot_ = -1;
  const String cookie = req.header("Cookie");
  const String prefix = "FR-SESSION=";
  const int start = cookie.indexOf(prefix);
  if (start < 0) return false;
  const int end = cookie.indexOf(';', start);
  const String token = cookie.substring(start + prefix.length(),
                                        end < 0 ? cookie.length() : end);
  if (!WebUiSessionPolicy::isHexToken(
          token.c_str(), token.length(), WebUiSessionPolicy::SESSION_TOKEN_HEX_LENGTH))
    return false;

  uint8_t raw[32] = {};
  for (size_t i = 0; i < sizeof(raw); ++i) {
    const char a = token[i * 2], b = token[i * 2 + 1];
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = hex(a), lo = hex(b);
    if (hi < 0 || lo < 0) return false;
    raw[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  RuntimeConfig config;
  if (!configSnapshot(config))
    return false;
  const uint32_t now = millis();
  const IPAddress ip = req.remoteIp();
  const uint32_t ipValue = static_cast<uint32_t>(ip[0]) |
                           (static_cast<uint32_t>(ip[1]) << 8) |
                           (static_cast<uint32_t>(ip[2]) << 16) |
                           (static_cast<uint32_t>(ip[3]) << 24);

  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    if (sessions_[i].inUse &&
        !WebUiSessionPolicy::isFresh(now, sessions_[i].issuedMs,
                                     config.webSessionTimeoutMs)) {
      sessions_[i].inUse = false;
    }
  }

  const int slot = findSessionSlot(raw, ipValue);
  if (slot < 0) return false;
  if (static_cast<int32_t>(now - authBlockedUntilMs_) < 0) return false;
  if (!WebUiSessionPolicy::isFresh(now, sessions_[slot].issuedMs,
                                   config.webSessionTimeoutMs)) {
    sessions_[slot].inUse = false;
    return false;
  }
  activeSessionSlot_ = static_cast<int8_t>(slot);
  return true;
}

bool WebUi::csrfValid(HttpdRequest& req) {
  if (req.method() != HTTP_POST && req.method() != HTTP_DELETE) return false;
  if (activeSessionSlot_ < 0 ||
      static_cast<size_t>(activeSessionSlot_) >= MAX_SESSIONS) return false;
  const SessionSlot& slot = sessions_[activeSessionSlot_];
  const String supplied = req.header("X-CSRF-Token");
  if (!WebUiSessionPolicy::isHexToken(
          supplied.c_str(), supplied.length(),
          WebUiSessionPolicy::CSRF_TOKEN_HEX_LENGTH)) {
    ++csrfFailures_;
    return false;
  }

  uint8_t diff = 0;
  const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(slot.csrf); ++i) {
    diff |= static_cast<uint8_t>(supplied[i * 2] ^ digits[slot.csrf[i] >> 4]);
    diff |= static_cast<uint8_t>(supplied[i * 2 + 1] ^ digits[slot.csrf[i] & 0x0F]);
  }
  if (diff != 0) {
    ++csrfFailures_;
    return false;
  }
  return true;
}

bool WebUi::issueSession(HttpdRequest& req, HttpdResponse& res) {
  const IPAddress ip = req.remoteIp();
  const uint32_t ipValue = static_cast<uint32_t>(ip[0]) |
                           (static_cast<uint32_t>(ip[1]) << 8) |
                           (static_cast<uint32_t>(ip[2]) << 16) |
                           (static_cast<uint32_t>(ip[3]) << 24);
  RuntimeConfig config;
  if (!configSnapshot(config))
    return false;

  const uint32_t now = millis();
  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    if (sessions_[i].inUse &&
        !WebUiSessionPolicy::isFresh(now, sessions_[i].issuedMs,
                                     config.webSessionTimeoutMs)) {
      sessions_[i].inUse = false;
    }
  }

  size_t slotIndex = MAX_SESSIONS;
  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    if (!sessions_[i].inUse) {
      slotIndex = i;
      break;
    }
  }
  if (slotIndex == MAX_SESSIONS) {
    slotIndex = 0;
    for (size_t i = 1; i < MAX_SESSIONS; ++i) {
      if (static_cast<int32_t>(sessions_[i].issuedMs - sessions_[slotIndex].issuedMs) < 0)
        slotIndex = i;
    }
    sessions_[slotIndex].inUse = false;
  }

  SessionSlot& slot = sessions_[slotIndex];
  for (size_t i = 0; i < sizeof(slot.secret); i += 4) {
    const uint32_t r = esp_random();
    memcpy(slot.secret + i, &r, min<size_t>(4, sizeof(slot.secret) - i));
  }
  for (size_t i = 0; i < sizeof(slot.csrf); i += 4) {
    const uint32_t r = esp_random();
    memcpy(slot.csrf + i, &r, min<size_t>(4, sizeof(slot.csrf) - i));
  }
  slot.issuedMs = now;
  slot.clientIp = ipValue;
  slot.inUse = true;
  activeSessionSlot_ = static_cast<int8_t>(slotIndex);

  uint8_t msg[8] = {};
  WebUiSessionPolicy::makeSessionMessage(ipValue, slot.issuedMs, msg);
  uint8_t token[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, slot.secret, sizeof(slot.secret),
                             msg, sizeof(msg), token, sizeof(token)) != 0) {
    slot.inUse = false;
    activeSessionSlot_ = -1;
    return false;
  }

  String hex;
  hex.reserve(64);
  const char* digits = "0123456789abcdef";
  for (uint8_t b : token) {
    hex += digits[b >> 4];
    hex += digits[b & 0x0F];
  }

  char cookie[160] = {};
  if (!WebUiSessionPolicy::buildSessionCookie(
          hex.c_str(), config.webSessionTimeoutMs / 1000, cookie,
          sizeof(cookie))) {
    slot.inUse = false;
    activeSessionSlot_ = -1;
    return false;
  }
  res.setHeader("Set-Cookie", cookie);
  return true;
}

String WebUi::csrfTokenHexForActiveSession() const {
  if (activeSessionSlot_ < 0 ||
      static_cast<size_t>(activeSessionSlot_) >= MAX_SESSIONS ||
      !sessions_[activeSessionSlot_].inUse)
    return String();
  const char* digits = "0123456789abcdef";
  String hex;
  hex.reserve(sizeof(sessions_[activeSessionSlot_].csrf) * 2);
  for (uint8_t b : sessions_[activeSessionSlot_].csrf) {
    hex += digits[b >> 4];
    hex += digits[b & 0x0F];
  }
  return hex;
}

void WebUi::auditAuth(HttpdRequest& req, bool success) {
  if (!storage.ready()) return;
  SpiLock spiLock(pdMS_TO_TICKS(50));
  if (!spiLock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  const char* path = "/LOG/WEB-AUTH.LOG";
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (f.size() >= Config::WEB_AUTH_LOG_ROTATE_BYTES) {
    f.close();
    const char* old = "/LOG/WEB-AUTH.1.LOG";
    if (SD.exists(old)) SD.remove(old);
    if (SD.exists(path)) SD.rename(path, old);
    f = SD.open(path, FILE_APPEND);
  }
  if (f) {
    const uint32_t now = millis();
    f.printf("%lu,%s,%s\n",
             static_cast<unsigned long>(now),
             req.remoteIp().toString().c_str(),
             success ? "AUTH_OK" : "AUTH_FAIL");
    f.close();
  }
}

bool WebUi::basicAuthMatches(HttpdRequest& req, const String& user,
                             const RuntimeConfig& config) {
  const String header = req.header("Authorization");
  if (!header.startsWith("Basic ")) return false;
  const String encoded = header.substring(6);
  if (encoded.length() == 0 || encoded.length() > 128) return false;

  uint8_t decoded[96] = {};
  size_t outLen = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &outLen,
                             reinterpret_cast<const uint8_t*>(encoded.c_str()),
                             encoded.length()) != 0)
    return false;
  decoded[outLen] = 0;
  const char* colon = reinterpret_cast<const char*>(memchr(decoded, ':', outLen));
  if (!colon) return false;
  const size_t userLen = static_cast<size_t>(colon - reinterpret_cast<const char*>(decoded));
  if (userLen != user.length() ||
      memcmp(decoded, user.c_str(), userLen) != 0)
    return false;
  const String password(reinterpret_cast<const char*>(colon + 1));
  return config.verifyWebPassword(password);
}

bool WebUi::auth(HttpdRequest& req, HttpdResponse& res) {
  const uint32_t now = millis();
  RuntimeConfig config;
  if (!configSnapshot(config)) {
    res.send503("configuration unavailable");
    return false;
  }

  constexpr uint32_t AUTH_ENTRY_TTL_MS = 5UL * 60UL * 1000UL;
  const String clientIp = req.remoteIp().toString();
  AuthThrottleEntry* entry = nullptr;
  AuthThrottleEntry* eviction = &authThrottle_[0];
  for (auto& candidate : authThrottle_) {
    const bool expired = !candidate.ip.isEmpty() &&
                         now - candidate.lastSeenMs >= AUTH_ENTRY_TTL_MS;
    if (candidate.ip == clientIp && !candidate.ip.isEmpty() && !expired) {
      entry = &candidate;
      break;
    }
    if (expired || candidate.lastSeenMs < eviction->lastSeenMs) eviction = &candidate;
  }
  if (!entry) {
    entry = eviction;
    *entry = AuthThrottleEntry{};
    entry->ip = clientIp;
  }
  entry->lastSeenMs = now;

  if (static_cast<int32_t>(now - entry->blockedUntilMs) < 0) {
    res.send429("too many authentication failures");
    return false;
  }

  if (static_cast<int32_t>(now - authBlockedUntilMs_) < 0) {
    res.send429("too many authentication failures");
    return false;
  }

  if (!sessionValid(req)) {
    if (!basicAuthMatches(req, config.webUser, config)) {
      if (now - entry->windowStartMs >= 60000U) {
        entry->windowStartMs = now;
        entry->failures = 0;
      }
      ++entry->failures;
      ++authFailures_;
      ++authFailureWindowCount_;
      auditAuth(req, false);

      if (entry->failures >= 5) {
        entry->blockedUntilMs = now + 30000U;
        entry->failures = 0;
      }
      if (now - authFailureWindowStartMs_ >= 60000U) {
        authFailureWindowStartMs_ = now;
        authFailures_ = 0;
      }
      if (authFailures_ >= 20) {
        authBlockedUntilMs_ = now + 30000U;
        authFailures_ = 0;
      }
      res.send401Basic("Login Required", "");
      return false;
    }

    entry->failures = 0;
    entry->windowStartMs = now;
    authFailures_ = 0;
    authFailureWindowCount_ = 0;
    authFailureWindowStartMs_ = now;
    if (!issueSession(req, res)) {
      auditAuth(req, false);
      res.send503("session initialization failed");
      return false;
    }
    auditAuth(req, true);
  }
  if (req.method() == HTTP_POST || req.method() == HTTP_DELETE) {
    if (config.csrfPolicy != 2 && config.csrfPolicy == 0 && !sameOrigin(req)) {
      res.send403("forbidden origin");
      return false;
    }
    if (config.csrfPolicy != 2 && !csrfValid(req)) {
      res.send403("invalid CSRF token");
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Handlers: root & meta
// ---------------------------------------------------------------------------
void WebUi::handleRoot(HttpdRequest& req, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.setHeader("X-Content-Type-Options", "nosniff");
  res.setHeader("X-Frame-Options", "DENY");
  res.setHeader("Referrer-Policy", "no-referrer");
  res.setHeader("Content-Security-Policy",
                "default-src 'self'; script-src 'self' 'unsafe-inline' https://unpkg.com; "
                "style-src 'self' 'unsafe-inline' https://unpkg.com; object-src 'none'; "
                "img-src 'self' data: https://*.tile.openstreetmap.org; "
                "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'");
  String page = FPSTR(INDEX_HTML);
  page.replace("__CSRF_TOKEN__", csrfTokenHexForActiveSession());
  Preferences themePrefs;
  String persistedTheme = "dark";
  if (themePrefs.begin("fieldradio", true)) { persistedTheme = themePrefs.getString("theme", "dark"); themePrefs.end(); }
  page.replace("__THEME_CLASS__", persistedTheme == "light" ? "light" : "");
  page.replace("__MQTT_TLS_DISABLED__", Config::mqttTlsIsMandatory() ? " disabled" : "");
  res.sendHtml(200, page);
}

void WebUi::handleApiVersion(HttpdRequest& req, HttpdResponse& res) {
  (void)req;
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, "{\"api\":1,\"protocol\":" +
                     String(Config::LORA_PROTOCOL_VERSION) +
                     ",\"secureBootV2\":" +
                     String(CONFIG_SECURE_BOOT_V2_ENABLED ? "true" : "false") +
                     ",\"flashEncryption\":" +
                     String(CONFIG_SECURE_FLASH_ENC_ENABLED ? "true" : "false") +
                     "}");
}

void WebUi::handleStatus(HttpdRequest& req, HttpdResponse& res) {
  (void)req;
  RuntimeConfig config;
  if (!configSnapshot(config)) {
    res.send503("configuration unavailable");
    return;
  }
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }

  String j = "{";
  j += "\"gps\":{\"valid\":" + String(gState.gps.valid ? "true":"false");
  j += ",\"lat\":" + String(gState.gps.lat,6);
  j += ",\"lon\":" + String(gState.gps.lon,6);
  j += ",\"alt\":" + String(gState.gps.alt,1);
  j += ",\"sat\":" + String(gState.gps.satellites);
  j += ",\"timeValid\":" + String(gState.gps.timeValid ? "true":"false");
  j += ",\"utcEpoch\":" + String(static_cast<unsigned long long>(gState.gps.utcEpoch)) + "},";
  j += "\"lora\":{\"ready\":" + String(gState.loraReady ? "true":"false");
  j += ",\"rssi\":" + String(gState.loraRssi) + ",\"snr\":" + String(gState.loraSnr,1);
  j += ",\"lqi\":" + String(lora.lqi());
  j += ",\"sf\":" + String(lora.currentDataRate());
  j += ",\"adr\":" + String(lora.adrEnabled() ? "true" : "false") + "},";
  j += "\"codec\":" + String(gState.codecReady ? "true":"false") + ",";
  j += "\"sensorDropped\":" + String(gState.sensorDropped) + ",";
  j += "\"sensorSpoolDepth\":" + String(gState.sensorSpoolDepth) + ",";
  j += "\"sensorSpoolEvictions\":" + String(gState.sensorSpoolEvictions) + ",";
  j += "\"sensorSpoolDrops\":" + String(gState.sensorSpoolDrops) + ",";
  j += "\"sensorSpoolRecovered\":" + String(gState.sensorSpoolRecovered) + ",";
  j += "\"peerMacFailures\":" + String(gState.peerMacFailures) + ",";
  j += "\"sd\":" + String(gState.storageReady ? "true":"false") + ",";
  j += "\"battery\":{\"available\":" + String(gState.batteryAvailable ? "true":"false");
  j += ",\"v\":";
  j += gState.batteryAvailable ? String(gState.batteryV, 2) : "null";
  j += ",\"low\":" + String(gState.batteryLow ? "true":"false");
  j += ",\"percent\":" + String(gState.batteryPercent);
  j += ",\"estimatedMinutes\":" + String(gState.batteryEstimatedMinutes);
  j += ",\"critical\":" + String(gState.batteryCritical ? "true":"false") + "},";
  j += "\"usbAudio\":" + String(gState.usbAudioReady ? "true":"false") + ",";
  j += "\"usbAudioActive\":" + String(gState.usbAudioActive ? "true":"false") + ",";
  j += "\"audioSource\":" + String(audio.recordSource()) + ",";
  j += "\"usbMuted\":" + String(gState.usbMuted ? "true":"false") + ",";
  j += "\"usbVolume\":" + String(gState.usbVolume) + ",";
  j += "\"usbMonitor\":" + String(gState.usbMonitor ? "true":"false") + ",";
  j += "\"usbPlaybackTransport\":" + String(gState.usbPlaybackTransport ? "true":"false") + ",";
  j += "\"aecEnabled\":" + String(gState.aecEnabled ? "true":"false") + ",";
  j += "\"usbSampleRate\":" + String(gState.usbSampleRate) + ",";
  j += "\"audioLoopback\":" + String(gState.audioLoopback ? "true":"false") + ",";
  j += "\"audioLevel\":{\"peak\":" + String(gState.audioPeak,3) +
       ",\"rms\":" + String(gState.audioRms,3) +
       ",\"clipped\":" + String(gState.audioClipped ? "true":"false") + "},";
  j += "\"ptt\":" + String(gState.ptt ? "true":"false") + ",";
  j += "\"sos\":" + String(gState.sos ? "true":"false") + ",";
  j += "\"recording\":" + String(gState.recording ? "true":"false") + ",";
  j += "\"rxActive\":" + String(gState.rxActive ? "true":"false") + ",";
  j += "\"recordingPaused\":" + String(gState.recordingPaused ? "true":"false") + ",\"playing\":" + String(gState.playing ? "true":"false") + ",\"playbackPaused\":" + String(gState.playbackPaused ? "true":"false") + ",\"playbackPositionMs\":" + String(gState.playbackPositionMs) + ",\"queueDepth\":" + String(gState.queueDepth) + ",\"vox\":" + String(gState.vox ? "true":"false") + ",\"voiceTxPackets\":" + String(gState.voiceTxPackets) + ",\"voiceRxPackets\":" + String(gState.voiceRxPackets) + ",\"voiceDrops\":" + String(gState.voiceDrops) + ",";
  j += "\"volume\":" + String(gState.volume) + ",";
  j += "\"runtimeAdvanced\":{\"mqttEnabled\":" + String(config.mqttEnabled ? "true" : "false") +
       ",\"wakePeriodSec\":" + String(config.wakePeriodSec) +
       ",\"staSsid\":\"" + jsonEscape(config.staSsid) + "\"" +
       ",\"staConfigured\":" + String(config.staSsid.length() > 0 ? "true" : "false") +
       ",\"deepSleepEnabled\":" + String(config.deepSleepEnabled ? "true" : "false") +
       ",\"deepSleepIdleMs\":" + String(config.deepSleepIdleMs) +
       ",\"deepSleepWakeGraceMs\":" + String(config.deepSleepWakeGraceMs) +
       ",\"criticalShutdownDelayMs\":" + String(config.criticalShutdownDelayMs) +
       ",\"batteryLowThreshold\":" + String(config.batteryLowThreshold, 3) +
       ",\"batteryCriticalThreshold\":" + String(config.batteryCriticalThreshold, 3) +
       ",\"classDEnabled\":" + String(config.classDEnabled ? "true" : "false") +
       ",\"classDBoostLevel\":" + String(config.classDBoostLevel) +
       ",\"mqttTlsMandatory\":" + String(Config::mqttTlsIsMandatory() ? "true" : "false") +
       ",\"classDHardwareEnabled\":" + String(Config::CLASS_D_ENABLED ? "true" : "false") +
       ",\"mqttHost\":\"" + jsonEscape(config.mqttHost) + "\"" +
       ",\"mqttPort\":" + String(config.mqttPort) +
       ",\"mqttTlsRequired\":" + String(config.mqttTlsRequired ? "true" : "false") +
       ",\"mqttReconnectMinMs\":" + String(config.mqttReconnectMinMs) +
       ",\"mqttReconnectMaxMs\":" + String(config.mqttReconnectMaxMs) +
       ",\"mqttTelemetryPeriodMs\":" + String(config.mqttTelemetryPeriodMs) +
       ",\"mqttHealthPeriodMs\":" + String(config.mqttHealthPeriodMs) +
       ",\"mqttRetainTelemetry\":" + String(config.mqttRetainTelemetry ? "true" : "false") +
       ",\"mqttRetainAvailability\":" + String(config.mqttRetainAvailability ? "true" : "false") +
       ",\"mqttCredentialRotationDays\":" + String(config.mqttCredentialRotationDays) +
       ",\"voxEnabled\":" + String(config.voxEnabled ? "true" : "false") +
       ",\"voxThreshold\":" + String(config.voxThreshold, 4) +
       ",\"voxHangMs\":" + String(config.voxHangMs) +
       ",\"aecEnabled\":" + String(config.aecEnabled ? "true" : "false") +
       ",\"usbMonitor\":" + String(config.usbMonitor ? "true" : "false") +
       ",\"usbPlaybackTransport\":" + String(config.usbPlaybackTransport ? "true" : "false") +
       ",\"audioLoopback\":" + String(config.audioLoopback ? "true" : "false") +
       ",\"loraAdrEnabled\":" + String(config.loraAdrEnabled ? "true" : "false") +
       ",\"loraHopEnabled\":" + String(config.loraHopEnabled ? "true" : "false") +
       ",\"loraHopChannelProfile\":" + String(config.loraHopChannelProfile) +
       ",\"loraRangeTestMode\":" + String(config.loraRangeTestMode ? "true" : "false") +
       ",\"sensorReaderEnabled\":" + String(config.sensorReaderEnabled ? "true" : "false") +
       ",\"sensorScanIntervalMs\":" + String(config.sensorScanIntervalMs) +
       ",\"sensorScanWindowMs\":" + String(config.sensorScanWindowMs) +
       ",\"sensorScanDurationMs\":" + String(config.sensorScanDurationMs) +
       ",\"sensorConnectTimeoutMs\":" + String(config.sensorConnectTimeoutMs) +
       ",\"sensorNodeEvictionMs\":" + String(config.sensorNodeEvictionMs) +
       ",\"sensorMaxNodes\":" + String(config.sensorMaxNodes) +
       ",\"sensorRequireEncryption\":" + String(config.sensorRequireEncryption ? "true" : "false") +
       ",\"blePairingEnabled\":" + String(config.blePairingEnabled ? "true" : "false") +
       ",\"blePairingFailureThreshold\":" + String(config.blePairingFailureThreshold) +
       ",\"blePairingBlockMs\":" + String(config.blePairingBlockMs) +
       ",\"sensorKeepAwake\":" + String(config.sensorKeepAwake ? "true" : "false") +
       ",\"webSessionTimeoutMs\":" + String(config.webSessionTimeoutMs) +
       ",\"webAuthRateLimitMs\":" + String(config.webAuthRateLimitMs) +
       ",\"csrfPolicy\":" + String(config.csrfPolicy) +
       ",\"blePairingPolicy\":" + String(config.blePairingPolicy) +
       ",\"ecdhRekeyPolicy\":" + String(config.ecdhRekeyPolicy) +
       ",\"replayWindowBits\":" + String(config.replayWindowBits) + "},";
  j += "\"voiceRxLost\":" + String(gState.voiceRxLost) + ",";
  j += "\"messageHistory\":" + String(gState.messageHistoryCount) + ",\"messageUnread\":" + String(gState.messageUnreadCount) + ",\"sosEscalated\":" + String(gState.sosEscalated ? "true" : "false") + ",";
  j += "\"loraLog\":" + String(gState.loraPacketLogCount) + ",";
  j += "\"healthAlerts\":" + String(gState.healthAlerts) + ",";
  j += "\"diagnostics\":{\"bootCount\":" + String(gState.bootCount) +
       ",\"wakeupCause\":" + String(gState.wakeupCause) +
       ",\"resetReason\":" + String(gState.resetReason) +
       ",\"brownout\":" + String(gState.brownoutReset ? "true" : "false") +
       ",\"heapLargestFree\":" + String(gState.heapLargestFree) +
       ",\"jammingDetected\":" + String(gState.jammingDetected ? "true" : "false") +
       ",\"noiseFloorDbm\":" + String(gState.noiseFloorDbm) +
       ",\"channelOccupancy\":" + String(gState.channelOccupancy) +
       ",\"antennaOk\":" + String(gState.antennaOk ? "true" : "false") +
       ",\"txRssi\":" + String(gState.txRssi) +
       ",\"antennaBaselineRssi\":" + String(gState.antennaBaselineRssi) + "},";
  j += "\"cpuTempC\":" + String(gState.cpuTempC, 1) +
       ",\"rangeTest\":" + String(gState.rangeTest ? "true" : "false") +
       ",\"batteryCalibrationDrift\":" +
       String(gState.batteryCalibrationDrift ? "true" : "false") + ",";
  j += "\"tx\":" + String(gState.txPackets) + ",";
  j += "\"rx\":" + String(gState.rxPackets) + ",";
  j += "\"msg\":\"" + jsonEscape(gState.lastMessage) + "\",";
  j += "\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += ",\"radioStats\":{\"forwardQueued\":" + String(lora.forwardQueued()) +
       ",\"forwardDrops\":" + String(lora.forwardDrops()) +
       ",\"forwardLastDropMs\":" + String(lora.forwardLastDropMs()) +
       ",\"fragmentEvictions\":" + String(lora.fragmentEvictions()) +
       ",\"fragmentDrops\":" + String(lora.fragmentDrops()) +
       ",\"dutyBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyBudgetUs())) +
       ",\"dutyMaxBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyMaxBudgetUs())) +
       ",\"gzipStalls\":" + String(storage.gzipStalls()) + "}";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Handlers: LoRaWAN
// ---------------------------------------------------------------------------
void WebUi::handleLoRaWANStatus(HttpdRequest& req, HttpdResponse& res) {
  (void)req;
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "{\"enabled\":" + String(config.lorawanEnabled ? "true" : "false") +
             ",\"mode\":" + String(config.lorawanMode) +
             ",\"region\":" + String(static_cast<uint8_t>(lorawan.regionalProfile())) +
             ",\"state\":" + String(static_cast<uint8_t>(lorawan.state())) +
             ",\"joined\":" + String(lorawan.isJoined() ? "true" : "false") +
             ",\"joining\":" + String(lorawan.isJoining() ? "true" : "false") +
             ",\"rssi\":" + String(lorawan.lastRssi()) +
             ",\"snr\":" + String(lorawan.lastSnr(), 1) +
             ",\"uplinkCount\":" + String(lorawan.uplinkCount()) +
             ",\"downlinkCount\":" + String(lorawan.downlinkCount()) +
             ",\"joinRetryCount\":" + String(lorawan.joinRetryCount()) +
             ",\"lastJoinMs\":" + String(lorawan.lastJoinAttemptMs()) +
             ",\"devEuiMasked\":\"" + jsonEscape(gState.lorawanDevEuiMasked) +
             "\",\"error\":\"" + jsonEscape(lorawan.lastError()) + "\"}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleLoRaWANConnect(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const bool ok = config.lorawanMode == 0 ? lorawan.connectOTAA() : lorawan.connectABP();
  res.sendText(ok ? 202 : 400, ok ? "LoRaWAN connect requested" : "LoRaWAN connect rejected");
}

void WebUi::handleLoRaWANDisconnect(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const bool ok = lorawan.disconnect();
  res.sendText(ok ? 202 : 400, ok ? "LoRaWAN disconnect requested" : "LoRaWAN disconnect rejected");
}

void WebUi::handleLoRaWANConfig(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  RuntimeConfig candidate;
  uint32_t configGenerationSnapshot = 0;
  if (!configSnapshot(candidate, configGenerationSnapshot)) {
    res.send503("configuration busy");
    return;
  }

  auto hexField = [](const String& v, size_t n) {
    if (v.length() != n) return false;
    for (size_t i = 0; i < v.length(); ++i) {
      const char c = v[i];
      if (!isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
  };
  auto parseU32 = [](const String& raw, uint32_t maxValue, uint32_t& out) {
    if (raw.isEmpty() || raw.length() > 10) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < raw.length(); ++i) {
      if (raw[i] < '0' || raw[i] > '9') return false;
      const uint32_t digit = static_cast<uint32_t>(raw[i] - '0');
      if (digit > maxValue || value > (maxValue - digit) / 10U) return false;
      value = value * 10U + digit;
    }
    out = value;
    return true;
  };

  if (req.hasArg("enabled")) {
    const String v = req.arg("enabled");
    if (v != "0" && v != "1") { res.sendText(400, "invalid enabled"); return; }
    candidate.lorawanEnabled = v == "1";
  }
  if (req.hasArg("mode")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("mode"), 1, v)) { res.sendText(400, "invalid mode"); return; }
    candidate.lorawanMode = static_cast<uint8_t>(v);
  }
  if (req.hasArg("region")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("region"), 3, v)) { res.sendText(400, "invalid region"); return; }
    candidate.lorawanRegion = static_cast<uint8_t>(v);
  }
  if (req.hasArg("deveui")) candidate.lorawanDevEui = req.arg("deveui");
  if (req.hasArg("joineui")) candidate.lorawanJoinEui = req.arg("joineui");
  if (req.hasArg("appkey")) candidate.lorawanAppKey = req.arg("appkey");
  if (req.hasArg("nwkskey")) candidate.lorawanNwkSKey = req.arg("nwkskey");
  if (req.hasArg("appskey")) candidate.lorawanAppSKey = req.arg("appskey");

  if (req.hasArg("devaddr")) {
    const String raw = req.arg("devaddr");
    if (!hexField(raw, 8)) { res.sendText(400, "invalid DevAddr"); return; }
    for (size_t i = 0; i < 4; ++i) {
      auto n = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = n(raw[i * 2]), lo = n(raw[i * 2 + 1]);
      if (hi < 0 || lo < 0) { res.sendText(400, "invalid DevAddr"); return; }
      candidate.lorawanDevAddr[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
  }
  if (req.hasArg("fport")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("fport"), 223, v) || v == 0) {
      res.sendText(400, "invalid FPort"); return;
    }
    candidate.lorawanFPort = static_cast<uint8_t>(v);
  }
  if (req.hasArg("period")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("period"), 86400, v) || v == 0) {
      res.sendText(400, "invalid period"); return;
    }
    candidate.lorawanUplinkPeriodSec = static_cast<uint16_t>(min<uint32_t>(v, 65535U));
  }

  if (candidate.lorawanEnabled) {
    if (!hexField(candidate.lorawanDevEui, 16) ||
        (candidate.lorawanMode == 0 &&
         (!hexField(candidate.lorawanJoinEui, 16) || !hexField(candidate.lorawanAppKey, 32))) ||
        (candidate.lorawanMode == 1 &&
         (!hexField(candidate.lorawanNwkSKey, 32) || !hexField(candidate.lorawanAppSKey, 32)))) {
      res.sendText(400, "invalid LoRaWAN credentials"); return;
    }
  }
  if (!candidate.validLoRaWAN()) {
    res.sendText(400, "invalid LoRaWAN configuration"); return;
  }

  RuntimeConfig previous;
  if (!configSnapshot(previous)) {
    res.send503("configuration busy");
    return;
  }
  const bool wasJoined = lorawan.isJoined();
  if (wasJoined) (void)lorawan.disconnect();
  if (!configCommit(candidate, configGenerationSnapshot)) {
    res.sendText(409, "configuration changed; retry");
    return;
  }
  if (candidate.lorawanRegion != previous.lorawanRegion)
    lorawan.setRegionalProfile(static_cast<RegionalProfile>(candidate.lorawanRegion));
  if (!candidate.lorawanEnabled) (void)lorawan.disconnect();
  auditConfigChange(previous, candidate, "lorawan-web");
  res.sendText(200, "LoRaWAN configuration saved");
}

void WebUi::handleLoRaWANUplink(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastMessageMs_, config.webAuthRateLimitMs)) return;
  const String text = req.arg("text");
  const String raw = req.arg("hex");
  uint8_t payload[Config::LORAWAN_MAX_PAYLOAD] = {};
  size_t len = 0;
  if (!raw.isEmpty()) {
    if ((raw.length() & 1U) || raw.length() > Config::LORAWAN_MAX_PAYLOAD * 2U) {
      res.sendText(400, "invalid hex payload"); return;
    }
    auto n = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i < raw.length() / 2; ++i) {
      const int hi = n(raw[i * 2]), lo = n(raw[i * 2 + 1]);
      if (hi < 0 || lo < 0) { res.sendText(400, "invalid hex payload"); return; }
      payload[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    len = raw.length() / 2;
  } else {
    if (text.isEmpty() || text.length() > Config::LORAWAN_MAX_PAYLOAD) {
      res.sendText(400, "invalid text payload"); return;
    }
    memcpy(payload, text.c_str(), text.length());
    len = text.length();
  }
  const String confirmed = req.arg("confirmed");
  const bool isConfirmed = confirmed == "1";
  const bool ok = lorawan.sendUplink(config.lorawanFPort, payload, len, isConfirmed);
  res.sendText(ok ? 202 : 409, ok ? "uplink queued" : "uplink rejected");
}

// ---------------------------------------------------------------------------
// Handlers: files
// ---------------------------------------------------------------------------
void WebUi::handleFiles(HttpdRequest& req, HttpdResponse& res) {
  const String dir = req.arg("dir");
  res.sendJson(200, storage.listJson(dir.isEmpty() ? "/REC" : dir));
}

void WebUi::handleDownload(HttpdRequest& req, HttpdResponse& res) {
  const String path = req.arg("path");
  if (!storage.isManagedAudioPath(path)) {
    res.sendText(400, "invalid path");
    return;
  }
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { res.send503("busy"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    res.send404("not found");
    return;
  }
  res.setHeader("Content-Disposition", "attachment; filename=\"" +
                path.substring(path.lastIndexOf('/') + 1) + "\"");
  if (!res.streamFile(f, "audio/wav")) {
    Serial.println("WebUI: streamFile failed");
  }
  f.close();
}

void WebUi::handleUpload(HttpdRequest& req, HttpdResponse& res) {
  uploadFailed_ = false;
  uploadBytes_ = 0;
  uploadPath_ = String();

  HttpdMultipart parser;
  auto fieldCb = [](const char*, const char*, const uint8_t*, size_t) -> bool {
    return true;
  };
  auto fileCb = [this](const char* name, const char* filename, const char* mime,
                       const uint8_t* data, size_t len,
                       bool firstChunk, bool lastChunk) -> bool {
    handleUploadChunk(name, filename, mime, data, len, firstChunk, lastChunk);
    return !uploadFailed_;
  };

  if (!parser.parse(req, Config::WEB_UPLOAD_MAX_BYTES + 4096, fieldCb, fileCb)) {
    uploadFailed_ = true;
  }
  if (uploadFailed_) {
    res.sendText(400, "upload failed");
    return;
  }
  res.sendText(200, "OK");
}

void WebUi::handleUploadChunk(const char* /*name*/, const char* filename,
                              const char* /*mime*/, const uint8_t* data,
                              size_t len, bool firstChunk, bool lastChunk) {
  if (uploadFailed_) return;

  if (firstChunk) {
    String name = filename ? String(filename) : String();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    uploadPath_ = "/REC/" + name;
    uploadBytes_ = 0;
    if (!storage.isManagedAudioPath(uploadPath_) || SD.exists(uploadPath_)) {
      uploadFailed_ = true;
      return;
    }
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (!spiLock.ok()) { uploadFailed_ = true; return; }
    uploadFile_ = SD.open(uploadPath_, FILE_WRITE);
    if (!uploadFile_) uploadFailed_ = true;
    return;
  }

  if (data != nullptr && len > 0) {
    if (uploadFailed_ || !uploadFile_ ||
        len > Config::WEB_UPLOAD_MAX_BYTES - uploadBytes_) {
      uploadFailed_ = true;
      return;
    }
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (!spiLock.ok() || uploadFile_.write(data, len) != len) {
      uploadFailed_ = true;
      return;
    }
    uploadBytes_ += len;
  }

  if (lastChunk) {
    if (uploadFile_) uploadFile_.close();
    if (!uploadFailed_ &&
        (uploadBytes_ == 0 || uploadBytes_ > Config::WEB_UPLOAD_MAX_BYTES ||
         !isValidUploadedWav(uploadPath_))) {
      uploadFailed_ = true;
    }
    if (uploadFailed_ && !uploadPath_.isEmpty()) {
      SpiLock spiLock(pdMS_TO_TICKS(100));
      if (spiLock.ok()) SD.remove(uploadPath_);
    }
  }
}

void WebUi::handleRename(HttpdRequest& req, HttpdResponse& res) {
  const String from = req.arg("from");
  const String to = req.arg("to");
  const bool ok = storage.renameFile(from, to);
  res.sendText(ok ? 200 : 400, ok ? "OK" : "FAIL");
}

// ---------------------------------------------------------------------------
// Handlers: messages
// ---------------------------------------------------------------------------
void WebUi::handleMessages(HttpdRequest& req, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  const String query = req.arg("q");
  uint64_t from = 0, to = UINT64_MAX;
  if (req.hasArg("from")) from = strtoull(req.arg("from").c_str(), nullptr, 10);
  if (req.hasArg("to")) to = strtoull(req.arg("to").c_str(), nullptr, 10);
  String j = "[";
  bool first = true;
  const size_t count = gState.messageHistoryCount;
  const size_t start = (gState.messageHistoryNext + Config::MESSAGE_HISTORY_SIZE - count) %
                       Config::MESSAGE_HISTORY_SIZE;
  for (size_t i = 0; i < count; ++i) {
    const auto& e = gState.messageHistory[(start + i) % Config::MESSAGE_HISTORY_SIZE];
    if (e.timestamp < from || e.timestamp > to) continue;
    if (!query.isEmpty() &&
        String(e.sourceId).indexOf(query) < 0 &&
        e.text.indexOf(query) < 0) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
         ",\"source\":" + String(e.sourceId) +
         ",\"read\":" + String(e.read ? "true" : "false") +
         ",\"text\":\"" + jsonEscape(e.text) + "\"}";
  }
  j += "]";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleMessageClear(HttpdRequest& /*req*/, HttpdResponse& res) {
  {
    StateLock lock(gState);
    if (!lock.ok()) { res.send503("busy"); return; }
    for (auto& e : gState.messageHistory) e = MessageHistoryEntry{};
    gState.messageHistoryNext = 0;
    gState.messageHistoryCount = 0;
    gState.messageUnreadCount = 0;
  }
  (void)lora.persistMessageHistory();
  res.sendText(200, "OK");
}

void WebUi::handleMessageRead(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("ts");
  const bool all = raw == "all";
  if (!all && (raw.isEmpty() || raw.length() > 20)) {
    res.sendText(400, "invalid timestamp"); return;
  }
  uint64_t ts = all ? 0 : strtoull(raw.c_str(), nullptr, 10);
  {
    StateLock lock(gState);
    if (!lock.ok()) { res.send503("busy"); return; }
    for (auto& e : gState.messageHistory) {
      if (e.timestamp != 0 && (all || e.timestamp == ts) && !e.read) {
        e.read = true;
        if (gState.messageUnreadCount) --gState.messageUnreadCount;
      }
    }
  }
  (void)lora.persistMessageHistory();
  res.sendText(200, "OK");
}

void WebUi::handleMessageReply(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastMessageMs_, config.webAuthRateLimitMs)) return;
  const String target = req.arg("source");
  String text;
  if (!req.bodyText(text, Config::LORA_FRAGMENT_MAX_BYTES)) {
    res.sendText(413, "reply too large"); return;
  }
  if (target.isEmpty() || target.length() > 10 || text.isEmpty() ||
      text.length() > Config::LORA_FRAGMENT_MAX_BYTES) {
    res.sendText(400, "invalid reply"); return;
  }

  uint32_t destination = 0;
  for (size_t i = 0; i < target.length(); ++i) {
    if (target[i] < '0' || target[i] > '9') {
      res.sendText(400, "invalid source"); return;
    }
    const uint32_t digit = static_cast<uint32_t>(target[i] - '0');
    if (destination > (UINT32_MAX - digit) / 10U) {
      res.sendText(400, "invalid source"); return;
    }
    destination = destination * 10U + digit;
  }
  if (destination == 0) {
    res.sendText(400, "invalid source"); return;
  }
  const bool ok = lora.sendTextTo(destination, text);
  res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
}

void WebUi::handleMessageExport(HttpdRequest& req, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  const String format = req.arg("format");
  String out;
  if (format.equalsIgnoreCase("csv")) {
    out = "timestamp,source,read,text\n";
    const size_t count = gState.messageHistoryCount;
    const size_t start = (gState.messageHistoryNext + Config::MESSAGE_HISTORY_SIZE - count) %
                         Config::MESSAGE_HISTORY_SIZE;
    for (size_t i = 0; i < count; ++i) {
      const auto& e = gState.messageHistory[(start + i) % Config::MESSAGE_HISTORY_SIZE];
      String text = e.text;
      text.replace("\"", "\"\"");
      out += String(static_cast<unsigned long long>(e.timestamp)) + "," +
             String(e.sourceId) + "," + (e.read ? "1" : "0") + ",\"" + text + "\"\n";
    }
    res.setHeader("Content-Disposition", "attachment; filename=\"messages.csv\"");
    res.sendCsv(200, out);
    return;
  }
  out = "[";
  const size_t count = gState.messageHistoryCount;
  const size_t start = (gState.messageHistoryNext + Config::MESSAGE_HISTORY_SIZE - count) %
                       Config::MESSAGE_HISTORY_SIZE;
  for (size_t i = 0; i < count; ++i) {
    if (i) out += ",";
    const auto& e = gState.messageHistory[(start + i) % Config::MESSAGE_HISTORY_SIZE];
    out += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
           ",\"source\":" + String(e.sourceId) +
           ",\"read\":" + String(e.read ? "true" : "false") +
           ",\"text\":\"" + jsonEscape(e.text) + "\"}";
  }
  out += "]";
  res.setHeader("Content-Disposition", "attachment; filename=\"messages.json\"");
  res.sendJson(200, out);
}

void WebUi::handleMessage(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastMessageMs_, config.webAuthRateLimitMs)) return;
  if (req.contentLength() > Config::MAX_WEB_BODY) {
    res.send413("payload too large");
    return;
  }
  String text;
  if (!req.bodyText(text, Config::MAX_WEB_BODY) || text.isEmpty() ||
      text.length() > Config::LORA_MAX_PACKET) {
    res.sendText(400, "invalid payload"); return;
  }
  const bool ok = lora.sendText(text);
  const bool acked = lora.textAcked();
  res.sendJson(ok ? 200 : 503,
               "{\"sent\":" + String(ok ? "true" : "false") +
               ",\"acked\":" + String(acked ? "true" : "false") + "}");
}

void WebUi::handleMessagePersist(HttpdRequest& /*req*/, HttpdResponse& res) {
  const bool ok = lora.persistMessageHistory();
  res.sendText(ok ? 200 : 503, ok ? "OK" : "persist failed");
}

// ---------------------------------------------------------------------------
// Handlers: record/message schedule
// ---------------------------------------------------------------------------
void WebUi::handleMessageSchedule(HttpdRequest& req, HttpdResponse& res) {
  if (!req.hasArg("at") || !req.hasArg("text")) {
    res.sendText(400, "at and text required"); return;
  }
  const String a = req.arg("at");
  char* end = nullptr;
  const unsigned long long at = strtoull(a.c_str(), &end, 10);
  const String text = req.arg("text");
  if (!end || *end != '\0' || at == 0 || text.isEmpty()) {
    res.sendText(400, "invalid schedule"); return;
  }
  const bool ok = lora.scheduleMessage(static_cast<uint64_t>(at), text);
  res.sendText(ok ? 200 : 503, ok ? "OK" : "schedule full");
}

void WebUi::handleMessageScheduleList(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, lora.scheduledMessagesJson());
}

void WebUi::handleMessageScheduleDelete(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("id");
  if (raw.isEmpty() || raw.length() > 10) {
    res.sendText(400, "invalid id"); return;
  }
  char* end = nullptr;
  const unsigned long value = strtoul(raw.c_str(), &end, 10);
  if (!end || *end != '\0' || value == 0 || value > UINT32_MAX) {
    res.sendText(400, "invalid id"); return;
  }
  const uint32_t id = static_cast<uint32_t>(value);
  res.sendText(lora.cancelScheduledMessage(id) ? 200 : 404, "OK");
}

void WebUi::handleRecordSchedule(HttpdRequest& req, HttpdResponse& res) {
  if (!req.hasArg("start") || !req.hasArg("duration")) {
    res.sendText(400, "start and duration required"); return;
  }
  char* startEnd = nullptr;
  char* durationEnd = nullptr;
  const String rawStart = req.arg("start");
  const String rawDuration = req.arg("duration");
  if (rawStart.isEmpty() || rawStart.length() > 20 ||
      rawDuration.isEmpty() || rawDuration.length() > 10) {
    res.sendText(400, "invalid schedule"); return;
  }
  const unsigned long long start = strtoull(rawStart.c_str(), &startEnd, 10);
  const unsigned long duration = strtoul(rawDuration.c_str(), &durationEnd, 10);
  if (!startEnd || *startEnd != '\0' ||
      !durationEnd || *durationEnd != '\0' ||
      start == 0 || duration == 0 || duration > Config::RECORD_MAX_SECONDS) {
    res.sendText(400, "invalid schedule"); return;
  }
  recordScheduleStart_ = static_cast<uint64_t>(start);
  recordScheduleDurationSec_ = static_cast<uint32_t>(duration);
  recordScheduleActive_ = true;
  res.sendText(200, "OK");
}

void WebUi::handleRecordScheduleGet(HttpdRequest& /*req*/, HttpdResponse& res) {
  String j = "{\"active\":" + String(recordScheduleActive_ ? "true" : "false") +
             ",\"start\":" + String(static_cast<unsigned long long>(recordScheduleStart_)) +
             ",\"duration\":" + String(recordScheduleDurationSec_) + "}";
  res.sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Handlers: SOS
// ---------------------------------------------------------------------------
void WebUi::handleSosFormat(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("list");
  if (raw.isEmpty() || raw.length() > 32) {
    res.sendText(400, "invalid format list"); return;
  }
  uint8_t mask = 0;
  int start = 0;
  while (start < static_cast<int>(raw.length())) {
    int comma = raw.indexOf(',', start);
    if (comma < 0) comma = raw.length();
    String item = raw.substring(start, comma);
    item.trim();
    if (item == "text") mask |= 1;
    else if (item == "aprs") mask |= 2;
    else if (item == "binary") mask |= 4;
    else { res.sendText(400, "unknown format"); return; }
    start = comma + 1;
  }
  res.sendText(lora.setSosFormats(mask) ? 200 : 400, "OK");
}

void WebUi::handleSos(HttpdRequest& req, HttpdResponse& res) {
  if (!rateLimit(req, res, lastSosMs_, Config::SOS_RATE_LIMIT_MS)) return;
  const String raw = req.arg("on");
  if (raw == "0") {
    if (!lora.cancelSOS()) {
      res.send503("SOS cancel failed");
      return;
    }
    res.sendText(200, "SOS OFF");
    return;
  }
  if (raw.isEmpty() || raw != "1") {
    res.sendText(400, "invalid sos");
    return;
  }
  bool ok = lora.sendSOS();
  if (ok) {
    StateLock lock(gState);
    if (lock.ok()) gState.sos = true;
  }
  res.sendText(ok ? 200 : 503, ok ? "SOS" : "FAIL");
}

void WebUi::handleSosStatus(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "{";
  j += "\"seq\":" + String(gState.sosSeq);
  j += ",\"acked\":" + String(gState.sosAcked ? "true" : "false");
  j += ",\"retries\":" + String(gState.sosRetries);
  j += ",\"lastAckMs\":" + String(gState.sosLastAckMs);
  j += ",\"source\":" + String(gState.sosLastAckSourceId);
  j += ",\"active\":" + String(gState.sos ? "true" : "false");
  j += ",\"escalated\":" + String(gState.sosEscalated ? "true" : "false");
  j += ",\"beacons\":" + String(gState.sosBeaconCount);
  j += ",\"ackedBy\":" + String(gState.sosAckedBy);
  j += ",\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleSosHistory(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "[";
  const size_t count = gState.sosHistoryCount;
  const size_t start = (gState.sosHistoryNext + RuntimeState::SOS_HISTORY_SIZE - count) %
                       RuntimeState::SOS_HISTORY_SIZE;
  for (size_t i = 0; i < count; ++i) {
    const auto& e = gState.sosHistory[(start + i) % RuntimeState::SOS_HISTORY_SIZE];
    if (i) j += ",";
    j += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
         ",\"seq\":" + String(e.seq) + ",\"event\":" + String(e.event) +
         ",\"peer\":" + String(e.peer) + "}";
  }
  j += "]";
  res.sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Handlers: selftest
// ---------------------------------------------------------------------------
void WebUi::handleSelfTest(HttpdRequest& /*req*/, HttpdResponse& res) {
  bool loraOk = false, codecOk = false, sdOk = false, gpsOk = false, batOk = false;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      loraOk = gState.loraReady;
      codecOk = gState.codecReady;
      sdOk = gState.storageReady;
      gpsOk = gState.gps.valid;
      batOk = gState.batteryAvailable;
    }
  }
  const bool pass = loraOk && codecOk && sdOk;
  selfTestMs_ = millis();
  selfTestResult_ = "{\"pass\":" + String(pass ? "true" : "false") +
    ",\"lora\":" + String(loraOk ? "true" : "false") +
    ",\"audio\":" + String(codecOk ? "true" : "false") +
    ",\"sd\":" + String(sdOk ? "true" : "false") +
    ",\"gps\":" + String(gpsOk ? "true" : "false") +
    ",\"battery\":" + String(batOk ? "true" : "false") + "}";
  res.sendJson(200, selfTestResult_);
}

void WebUi::handleSelfTestResult(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.sendJson(200, selfTestResult_.isEmpty()
                       ? "{\"pass\":false,\"error\":\"not run\"}"
                       : selfTestResult_);
}

// ---------------------------------------------------------------------------
// Handlers: UI prefs
// ---------------------------------------------------------------------------
void WebUi::handleLang(HttpdRequest& req, HttpdResponse& res) {
  const String code = req.arg("set");
  if (code != "en" && code != "id") {
    res.sendText(400, "invalid language"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) {
    res.send503("NVS unavailable"); return;
  }
  const bool ok = prefs.putString("lang", code) > 0;
  prefs.end();
  res.sendText(ok ? 200 : 503, ok ? "OK" : "NVS save failed");
}

void WebUi::handleTheme(HttpdRequest& req, HttpdResponse& res) {
  const String mode = req.arg("mode");
  if (mode != "dark" && mode != "light") {
    res.sendText(400, "invalid theme"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) {
    res.send503("NVS unavailable"); return;
  }
  const bool ok = prefs.putString("theme", mode) > 0;
  prefs.end();
  res.sendText(ok ? 200 : 503, ok ? "OK" : "NVS save failed");
}

// ---------------------------------------------------------------------------
// Handlers: diagnostics
// ---------------------------------------------------------------------------
void WebUi::handleNeighbors(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, lora.neighborsJson());
}

void WebUi::handleRoutes(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, lora.routesJson());
}

void WebUi::handleDedupStats(HttpdRequest& /*req*/, HttpdResponse& res) {
  String j = "{\"hits\":" + String(lora.dedupHits()) +
             ",\"misses\":" + String(lora.dedupMisses()) +
             ",\"cacheSize\":" + String(Config::LORA_DEDUP_CACHE_SIZE) +
             ",\"evictions\":" + String(lora.dedupEvictions()) +
             ",\"replayRejects\":" + String(lora.replayRejects()) + "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleForwardStats(HttpdRequest& /*req*/, HttpdResponse& res) {
  String j = "{\"queued\":" + String(lora.forwardQueued()) +
             ",\"dropped\":" + String(lora.forwardDrops()) +
             ",\"lastDropMs\":" + String(lora.forwardLastDropMs()) +
             ",\"fragmentEvictions\":" + String(lora.fragmentEvictions()) +
             ",\"fragmentDrops\":" + String(lora.fragmentDrops()) +
             ",\"dutyBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyBudgetUs())) +
             ",\"dutyMaxBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyMaxBudgetUs())) +
             ",\"gzipStalls\":" + String(storage.gzipStalls()) + "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleAuthStats(HttpdRequest& /*req*/, HttpdResponse& res) {
  String j = "{\"failures\":" + String(authFailureWindowCount_) +
             ",\"blockedUntilMs\":" + String(authBlockedUntilMs_) +
             ",\"windowStartMs\":" + String(authFailureWindowStartMs_) +
             ",\"csrfFailures\":" + String(csrfFailures_) + "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleNvs(HttpdRequest& req, HttpdResponse& res) {
  const String key = req.arg("key");
  static const char* const allowed[] = {
    "cfgver", "freq", "bw", "sf", "cr", "sync", "power", "volume",
    "audsrc", "recqual", "batcal", "callsign", "theme", "bhealth_v", "bcycles", "bsamples"
  };
  bool allowedKey = false;
  for (const char* k : allowed)
    if (key == k) { allowedKey = true; break; }
  if (!allowedKey || key.length() > 15) {
    res.sendText(400, "key not allowed"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", true)) {
    res.send503("NVS unavailable"); return;
  }
  String value;
  if (key == "callsign" || key == "theme") value = prefs.getString(key.c_str(), "");
  else if (key == "freq" || key == "bw" || key == "batcal")
    value = String(prefs.getFloat(key.c_str(), 0.0f), 5);
  else if (key == "power")
    value = String(static_cast<int>(prefs.getChar(key.c_str(), 0)));
  else
    value = String(static_cast<unsigned long>(prefs.getUInt(key.c_str(), 0)));
  prefs.end();
  res.sendJson(200, "{\"key\":\"" + jsonEscape(key) + "\",\"value\":\"" + jsonEscape(value) + "\"}");
}

void WebUi::handleConfigMigrate(HttpdRequest& /*req*/, HttpdResponse& res) {
  RuntimeConfig candidate{};
  const bool ok = configSnapshot(candidate) && configCommit(candidate);
  res.sendText(ok ? 200 : 503, ok ? "OK" : "migration failed");
}

void WebUi::handleDiagFull(HttpdRequest& /*req*/, HttpdResponse& res) {
  RuntimeConfig config{};
  if (!configSnapshot(config)) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"configuration snapshot unavailable\"}");
    return;
  }

  uint32_t wdt[5] = {};
  uint32_t heapFree = 0, heapLargest = 0;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      std::memcpy(wdt, gState.wdtResetCounts, sizeof(wdt));
      heapFree = gState.heapFree;
      heapLargest = gState.heapLargestFree;
    }
  }

  String j = "{\"ok\":true";
  j += ",\"spool\":" + sensorSpool.statusJson();
  j += ",\"dedup\":{\"hits\":" + String(lora.dedupHits());
  j += ",\"misses\":" + String(lora.dedupMisses());
  j += ",\"evictions\":" + String(lora.dedupEvictions());
  j += ",\"replayRejects\":" + String(lora.replayRejects()) + "}";
  j += ",\"ack\":" + lora.sensorAckStatusJson();
  j += ",\"journal\":" + mqtt.deliveryJournalStatusJson();
  j += ",\"configTxn\":" + configTxnJournalStatusJson();
  j += ",\"config\":{\"generation\":" + String(static_cast<unsigned long>(configGeneration()));
  j += ",\"mqttEnabled\":" + String(config.mqttEnabled ? "true" : "false");
  j += ",\"mqttTlsRequired\":" + String(config.mqttTlsRequired ? "true" : "false");
  j += ",\"lorawanEnabled\":" + String(config.lorawanEnabled ? "true" : "false");
  j += ",\"lorawanMode\":" + String(config.lorawanMode);
  j += ",\"ecdhRekeyPolicy\":" + String(config.ecdhRekeyPolicy);
  j += ",\"sensorReaderEnabled\":" + String(config.sensorReaderEnabled ? "true" : "false");
  j += ",\"queuePolicy\":\"" +
       String(bleSensorReader.sensorReader().queuePolicy() ==
                      SensorReader::SampleQueuePolicy::DROP_OLDEST
                  ? "DROP_OLDEST" : "DROP_NEWEST") + "\"";
  j += ",\"secretsRedacted\":true}";
  j += ",\"wdt\":{\"gnss\":" + String(static_cast<unsigned long>(wdt[0]));
  j += ",\"lora\":" + String(static_cast<unsigned long>(wdt[1]));
  j += ",\"audio\":" + String(static_cast<unsigned long>(wdt[2]));
  j += ",\"web\":" + String(static_cast<unsigned long>(wdt[3]));
  j += ",\"lorawan\":" + String(static_cast<unsigned long>(wdt[4])) + "}";
  j += ",\"heap\":{\"free\":" + String(static_cast<unsigned long>(heapFree));
  j += ",\"largestFree\":" + String(static_cast<unsigned long>(heapLargest)) + "}";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleHealthLog(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "[";
  const size_t count = gState.healthLogCount;
  const size_t start = (gState.healthLogNext + Config::HEALTH_LOG_SIZE - count) %
                       Config::HEALTH_LOG_SIZE;
  for (size_t i = 0; i < count; ++i) {
    const auto& e = gState.healthLog[(start + i) % Config::HEALTH_LOG_SIZE];
    if (i) j += ",";
    j += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
         ",\"stalledMask\":" + String(e.stalledMask) +
         ",\"heap\":" + String(e.heapFree) +
         ",\"gnssStack\":" + String(e.gnssStackMin) +
         ",\"loraStack\":" + String(e.loraStackMin) +
         ",\"audioStack\":" + String(e.audioStackMin) +
         ",\"webStack\":" + String(e.webStackMin) +
         ",\"wdtResetCounts\":[" + String(gState.wdtResetCounts[0]) + "," +
         String(gState.wdtResetCounts[1]) + "," + String(gState.wdtResetCounts[2]) + "," +
         String(gState.wdtResetCounts[3]) + "]}";
  }
  j += "]";
  res.sendJson(200, j);
}

void WebUi::handleLoraLog(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "[";
  const size_t count = gState.loraPacketLogCount;
  const size_t start = (gState.loraPacketLogNext + Config::LORA_PACKET_LOG_SIZE - count) %
                       Config::LORA_PACKET_LOG_SIZE;
  for (size_t i = 0; i < count; ++i) {
    const auto& e = gState.loraPacketLog[(start + i) % Config::LORA_PACKET_LOG_SIZE];
    if (i) j += ",";
    j += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
         ",\"dir\":\"" + String(e.tx ? "TX" : "RX") +
         "\",\"type\":" + String(e.type) + ",\"seq\":" + String(e.seq) +
         ",\"source\":" + String(e.sourceId) + ",\"rssi\":" + String(e.rssi) +
         ",\"snr\":" + String(e.snr,1) + ",\"ttl\":" + String(e.ttl) + "}";
  }
  j += "]";
  res.sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Handlers: capture / ADR / HOP
// ---------------------------------------------------------------------------
void WebUi::handleCaptureStart(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("duration");
  if (raw.isEmpty() || raw.length() > 8) {
    res.sendText(400, "invalid duration"); return;
  }
  char* end = nullptr;
  const unsigned long ms = strtoul(raw.c_str(), &end, 10);
  if (!end || *end != '\0' || ms == 0 || ms > Config::CAPTURE_MAX_DURATION_MS) {
    res.sendText(400, "invalid duration"); return;
  }
  const bool ok = lora.captureStart(static_cast<uint32_t>(ms));
  res.sendText(ok ? 200 : 503, ok ? "OK" : "capture unavailable");
}

void WebUi::handleCaptureStop(HttpdRequest& /*req*/, HttpdResponse& res) {
  const bool ok = lora.captureStop();
  res.sendText(ok ? 200 : 503, ok ? "OK" : "capture unavailable");
}

void WebUi::handleCaptureDump(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, lora.captureDumpJson());
}

void WebUi::handleAdr(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") {
    res.sendText(400, "invalid adr"); return;
  }
  const bool ok = lora.setAdrEnabled(raw == "1");
  res.sendText(ok ? 200 : 503, ok ? "OK" : "ADR unavailable");
}

void WebUi::handleHopSync(HttpdRequest& req, HttpdResponse& res) {
  const String source = req.arg("source");
  if (source != "gps" && source != "internal") {
    res.sendText(400, "invalid source"); return;
  }
  lora.setHopSyncSource(source == "gps");
  res.sendText(200, "OK");
}

void WebUi::handleHopSuggest(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  uint8_t suggested[Config::HOP_CHANNEL_MAX] = {};
  const size_t n = lora.scannerSuggestBestChannels(
      suggested, Config::HOP_CHANNEL_MAX);
  if (n == 0) {
    res.sendText(409, "no scan results");
    return;
  }
  {
    StateLock lock(gState);
    if (!lock.ok()) { res.send503("busy"); return; }
    for (size_t i = 0; i < Config::HOP_CHANNEL_MAX; ++i)
      gState.hopChannelList[i] = (i < n) ? suggested[i] : 0;
    gState.hopChannelCount = static_cast<uint8_t>(n);
  }
  String j = "{\"count\":" + String(static_cast<unsigned>(n)) +
             ",\"channels\":[";
  for (size_t i = 0; i < n; ++i) {
    if (i) j += ",";
    j += String(suggested[i]);
  }
  j += "]}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleHopStatus(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "{";
  j += "\"enabled\":" + String(gState.hopEnabled ? "true" : "false");
  j += ",\"count\":" + String(gState.hopChannelCount);
  j += ",\"dwellMs\":" + String(Config::HOP_DWELL_MS);
  j += ",\"channels\":[";
  for (size_t i = 0; i < gState.hopChannelCount &&
                     i < Config::HOP_CHANNEL_MAX; ++i) {
    if (i) j += ",";
    j += String(gState.hopChannelList[i]);
  }
  j += "],\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleHopEnable(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") {
    res.sendText(400, "invalid enable"); return;
  }
  const bool on = (raw == "1");
  {
    StateLock lock(gState);
    if (!lock.ok()) { res.send503("busy"); return; }
    if (on && gState.hopChannelCount == 0) {
      res.sendText(409, "no channels suggested"); return;
    }
  }
  RuntimeConfig candidate{};
  if (!configSnapshot(candidate)) { res.send503("configuration snapshot unavailable"); return; }
  candidate.loraHopEnabled = on;
  if (!configCommit(candidate)) { res.send503("NVS save failed"); return; }
  {
    StateLock lock(gState);
    if (lock.ok()) gState.hopEnabled = on;
  }
  res.sendText(200, on ? "HOP ENABLED" : "HOP DISABLED");
}

void WebUi::handleHopSetChannels(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const String raw = req.arg("list");
  if (raw.isEmpty() || raw.length() > 32) {
    res.sendText(400, "invalid channel list"); return;
  }
  uint8_t channels[Config::HOP_CHANNEL_MAX] = {};
  size_t count = 0;
  size_t start = 0;
  while (start <= raw.length()) {
    size_t comma = raw.indexOf(',', start);
    if (comma < 0) comma = raw.length();
    String token = raw.substring(start, comma);
    token.trim();
    if (token.isEmpty() || token.length() > 2) {
      res.sendText(400, "invalid channel"); return;
    }
    int value = 0;
    for (size_t i = 0; i < token.length(); ++i) {
      if (token[i] < '0' || token[i] > '9') {
        res.sendText(400, "invalid channel"); return;
      }
      value = value * 10 + (token[i] - '0');
    }
    if (value < 0 || value >= Config::HOP_CHANNEL_MAX) {
      res.sendText(400, "channel out of range"); return;
    }
    for (size_t i = 0; i < count; ++i) {
      if (channels[i] == static_cast<uint8_t>(value)) {
        res.sendText(400, "duplicate channel"); return;
      }
    }
    if (count >= Config::HOP_CHANNEL_MAX) {
      res.sendText(400, "too many channels"); return;
    }
    channels[count++] = static_cast<uint8_t>(value);
    if (comma == raw.length()) break;
    start = comma + 1;
  }
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  for (size_t i = 0; i < Config::HOP_CHANNEL_MAX; ++i)
    gState.hopChannelList[i] = i < count ? channels[i] : 0;
  gState.hopChannelCount = static_cast<uint8_t>(count);
  res.sendText(200, "OK");
}

// ---------------------------------------------------------------------------
// Handlers: scanner / range test
// ---------------------------------------------------------------------------
void WebUi::handleScanStatus(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "{";
  j += "\"active\":" + String(gState.scannerActive ? "true" : "false");
  j += ",\"mode\":" + String(gState.scannerMode);
  j += ",\"sweepInProgress\":" + String(gState.scannerSweepInProgress ? "true" : "false");
  j += ",\"sweep\":" + String(gState.scannerSweepCount);
  j += ",\"channels\":" + String(gState.scannerChannelCount);
  j += ",\"dwellMs\":" + String(gState.scannerDwellMs);
  j += ",\"lastSweepMs\":" + String(gState.scannerLastSweepMs);
  j += ",\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleScanStart(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const String modeRaw = req.arg("mode");
  const String dwellRaw = req.arg("dwell");
  if (modeRaw != "1" && modeRaw != "2") {
    res.sendText(400, "invalid mode"); return;
  }
  uint16_t dwell = Config::SCANNER_DEFAULT_DWELL_MS;
  if (!dwellRaw.isEmpty()) {
    for (size_t i = 0; i < dwellRaw.length(); ++i) {
      if (dwellRaw[i] < '0' || dwellRaw[i] > '9') {
        res.sendText(400, "invalid dwell"); return;
      }
    }
    const long v = dwellRaw.toInt();
    if (v < static_cast<long>(Config::SCANNER_MIN_DWELL_MS) ||
        v > static_cast<long>(Config::SCANNER_MAX_DWELL_MS)) {
      res.sendText(400, "dwell out of range"); return;
    }
    dwell = static_cast<uint16_t>(v);
  }
  const bool ok = lora.scannerStart(
      static_cast<uint8_t>(modeRaw == "2" ? 2 : 1), dwell);
  res.sendText(ok ? 200 : 409, ok ? "SCAN" : "FAIL");
}

void WebUi::handleScanStop(HttpdRequest& /*req*/, HttpdResponse& res) {
  const bool ok = lora.scannerStop();
  res.sendText(ok ? 200 : 409, ok ? "STOP" : "FAIL");
}

void WebUi::handleScanResults(HttpdRequest& /*req*/, HttpdResponse& res) {
  ChannelScanResult results[Config::SCANNER_MAX_CHANNELS] = {};
  size_t count = 0;
  lora.scannerGetResults(results, count);
  String j = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i) j += ",";
    const ChannelScanResult& r = results[i];
    j += "{\"freq\":" + String(r.freqMHz, 3) +
         ",\"rssiAvg\":" + String(r.rssiAvgDbm) +
         ",\"rssiPeak\":" + String(r.rssiPeakDbm) +
         ",\"snr\":" + String(r.snrDb, 1) +
         ",\"occupancy\":" + String(r.occupancyPercent) +
         ",\"preamble\":" + String(r.preambleCount) +
         ",\"ts\":" + String(r.timestamp) + "}";
  }
  j += "]";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleRangeTest(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") {
    res.sendText(400, "invalid range-test");
    return;
  }
  RuntimeConfig candidate;
  uint32_t generation = 0;
  if (!configSnapshot(candidate, generation)) {
    res.send503("configuration busy");
    return;
  }
  candidate.loraRangeTestMode = raw == "1";
  if (!configCommit(candidate, generation)) {
    res.sendText(409, "configuration changed; retry");
    return;
  }
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  gState.rangeTest = candidate.loraRangeTestMode;
  res.sendText(200, gState.rangeTest ? "RANGE TEST ON" : "RANGE TEST OFF");
}

void WebUi::handleRangeTestStatus(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, lora.rangeTestStatusJson());
}

// ---------------------------------------------------------------------------
// Handlers: radio / battery / storage
// ---------------------------------------------------------------------------
void WebUi::handleRadioHistory(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "[";
  const size_t count = gState.radioHistoryCount;
  const size_t start = (gState.radioHistoryNext + RuntimeState::RADIO_HISTORY_SIZE - count) %
                       RuntimeState::RADIO_HISTORY_SIZE;
  for (size_t i = 0; i < count; ++i) {
    const size_t idx = (start + i) % RuntimeState::RADIO_HISTORY_SIZE;
    if (i) j += ",";
    j += "{\"ms\":" + String(gState.radioHistoryMs[idx]) +
         ",\"rssi\":" + String(gState.rssiHistory[idx]) +
         ",\"snr\":" + String(gState.snrHistory[idx], 1) + "}";
  }
  j += "]";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleRadioTune(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const String raw = req.arg("freq");
  char* end = nullptr;
  const float freq = strtof(raw.c_str(), &end);
  if (!end || *end != '\0' || !isfinite(freq) ||
      freq < Config::LORA_MIN_FREQ_MHZ || freq > Config::LORA_MAX_FREQ_MHZ) {
    res.sendText(400, "frequency outside configured legal band"); return;
  }
  const bool ok = lora.manualTune(freq);
  res.sendText(ok ? 200 : 503, ok ? "OK" : "TUNE FAILED");
}

void WebUi::handleRadioStats(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  const size_t n = gState.radioHistoryCount;
  if (n == 0) {
    res.sendJson(200, "{\"count\":0,\"rssiAvg\":null,\"rssiMin\":null,\"rssiMax\":null,\"snrAvg\":null,\"snrMin\":null,\"snrMax\":null}");
    return;
  }
  int32_t rssiSum = 0, rssiMin = 127, rssiMax = -127;
  double snrSum = 0.0, snrMin = 1000.0, snrMax = -1000.0;
  const size_t start = (gState.radioHistoryNext + RuntimeState::RADIO_HISTORY_SIZE - n) %
                       RuntimeState::RADIO_HISTORY_SIZE;
  for (size_t i = 0; i < n; ++i) {
    const size_t k = (start + i) % RuntimeState::RADIO_HISTORY_SIZE;
    const int r = gState.rssiHistory[k];
    const double snr = gState.snrHistory[k];
    rssiSum += r; rssiMin = min(rssiMin, r); rssiMax = max(rssiMax, r);
    snrSum += snr; snrMin = min(snrMin, snr); snrMax = max(snrMax, snr);
  }
  String j = "{\"count\":" + String(n) +
             ",\"rssiAvg\":" + String(static_cast<float>(rssiSum) / n, 1) +
             ",\"rssiMin\":" + String(rssiMin) +
             ",\"rssiMax\":" + String(rssiMax) +
             ",\"snrAvg\":" + String(static_cast<float>(snrSum / n), 1) +
             ",\"snrMin\":" + String(static_cast<float>(snrMin), 1) +
             ",\"snrMax\":" + String(static_cast<float>(snrMax), 1) + "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleRfDetector(HttpdRequest& /*req*/, HttpdResponse& res) {
  const RfDetector& detector = lora.rfDetector();
  String j = "{\"forwardDbm\":" + String(detector.lastForwardDbm(), 2) +
             ",\"reflectedDbm\":" + String(detector.lastReflectedDbm(), 2) +
             ",\"vswr\":" + String(detector.lastVswr(), 2) +
             ",\"healthy\":" + String(detector.healthy() ? "true" : "false") + "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleBatteryHistory(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "[";
  const size_t n = gState.batteryHistoryCount;
  const size_t start = (gState.batteryHistoryNext +
                        RuntimeState::BATTERY_HISTORY_SIZE - n) %
                       RuntimeState::BATTERY_HISTORY_SIZE;
  for (size_t i = 0; i < n; ++i) {
    if (i) j += ",";
    const size_t k = (start + i) % RuntimeState::BATTERY_HISTORY_SIZE;
    j += "{\"ms\":" + String(gState.batteryHistoryMs[k]) +
         ",\"v\":" + String(gState.batteryHistoryV[k], 3) +
         ",\"percent\":" + String(gState.batteryHistoryPercent[k]) + "}";
  }
  j += "]";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleBatteryCalibrate(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("voltage");
  char* end = nullptr;
  const float actual = strtof(raw.c_str(), &end);
  if (!end || *end != '\0' || !isfinite(actual) || actual < 2.5f || actual > 6.0f) {
    res.sendText(400, "invalid voltage"); return;
  }
  float measured = NAN;
  {
    StateLock lock(gState);
    if (!lock.ok() || !gState.batteryAvailable || !isfinite(gState.batteryV)) {
      res.sendText(409, "battery measurement unavailable"); return;
    }
    measured = gState.batteryV;
  }
  if (measured <= 0.1f) { res.sendText(409, "invalid current measurement"); return; }
  RuntimeConfig candidate{};
  if (!configSnapshot(candidate)) { res.send503("configuration snapshot unavailable"); return; }
  candidate.batteryCalibration *= actual / measured;
  if (!isfinite(candidate.batteryCalibration) ||
      candidate.batteryCalibration < 0.5f || candidate.batteryCalibration > 1.5f ||
      !configCommit(candidate)) {
    res.send503("calibration save failed"); return;
  }
  res.sendText(200, "Battery calibration saved: " +
                    String(candidate.batteryCalibration, 5));
}

void WebUi::handleStorageInfo(HttpdRequest& /*req*/, HttpdResponse& res) {
  const uint64_t total = storage.totalBytes();
  const uint64_t used = storage.usedBytes();
  if (!total) { res.send503("storage unavailable"); return; }
  String j = "{\"total\":" + String(static_cast<unsigned long long>(total)) +
             ",\"used\":" + String(static_cast<unsigned long long>(used)) +
             ",\"free\":" + String(static_cast<unsigned long long>(total > used ? total-used : 0)) + "}";
  res.sendJson(200, j);
}

void WebUi::handleChecksum(HttpdRequest& req, HttpdResponse& res) {
  const String path = req.arg("path");
  if (!storage.isSafePath(path)) {
    res.sendText(400, "invalid path"); return;
  }
  uint32_t crc = 0; uint64_t size = 0;
  if (!storage.checksumFile(path, crc, size)) {
    res.send404("checksum failed"); return;
  }
  res.sendJson(200, "{\"size\":" + String(static_cast<unsigned long long>(size)) +
                    ",\"crc32\":\"" + String(crc, HEX) + "\"}");
}

void WebUi::handleChecksumSha256(HttpdRequest& req, HttpdResponse& res) {
  const String path = req.arg("path");
  if (!storage.isSafePath(path)) { res.sendText(400, "invalid path"); return; }
  String digest;
  uint64_t size = 0;
  if (!storage.sha256File(path, digest, size)) {
    res.send404("checksum failed");
    return;
  }
  res.sendJson(200, "{\"path\":\"" + jsonEscape(path) +
                    "\",\"size\":" + String(static_cast<unsigned long long>(size)) +
                    ",\"sha256\":\"" + digest + "\"}");
}

void WebUi::handleLogExport(HttpdRequest& req, HttpdResponse& res) {
  const String path = req.arg("file");
  if (!storage.isSafePath(path) || !path.startsWith("/LOG/")) {
    res.sendText(400, "invalid log path"); return;
  }
  if (req.arg("format") != "gz") {
    res.sendText(400, "only gz supported"); return;
  }
  const String outPath = "/LOG/.web-export.gz";
  if (!storage.exportGzip(path, outPath)) {
    res.send404("compression failed"); return;
  }
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { res.send503("storage busy"); return; }
  File f = SD.open(outPath, FILE_READ);
  if (!f) { res.send404("export missing"); return; }
  res.setHeader("Content-Disposition", "attachment; filename=\"fieldradio-log.gz\"");
  (void)res.streamFile(f, "application/gzip");
  f.close();
  SD.remove(outPath);
}

// ---------------------------------------------------------------------------
// Handlers: audio
// ---------------------------------------------------------------------------
void WebUi::handlePtt(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastPttMs_, config.webAuthRateLimitMs)) return;
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") {
    res.sendText(400, "invalid ptt");
    return;
  }
  const bool on = raw == "1";
  if (on) {
    if (!audio.startRecording()) {
      res.send503("PTT recording start failed");
      return;
    }
  } else {
    if (!audio.stopRecording()) {
      res.send503("PTT recording stop failed");
      return;
    }
  }
  StateLock lock(gState);
  if (!lock.ok()) { res.sendEmpty(503); return; }
  gState.ptt = on;
  res.sendText(200, on ? "PTT ON - recording" : "PTT OFF - recording stopped");
}

void WebUi::handleRecord(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") {
    res.sendText(400, "invalid record");
    return;
  }
  const bool on = raw == "1";
  bool ok = on ? audio.startRecording() : audio.stopRecording();
  res.sendText(ok ? 200 : 503, on ? "REC" : "STOP");
}

void WebUi::handlePlay(HttpdRequest& req, HttpdResponse& res) {
  String path = req.arg("path");
  bool ok = audio.playFile(path);
  res.sendText(ok ? 200 : 400, ok ? "PLAY" : "FAIL");
}

void WebUi::handleStop(HttpdRequest& /*req*/, HttpdResponse& res) {
  audio.stopPlayback();
  res.sendText(200, "STOP");
}

void WebUi::handlePause(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") { res.sendText(400, "invalid pause"); return; }
  const bool ok = audio.pausePlayback(raw == "1");
  res.sendText(ok ? 200 : 409, ok ? "OK" : "FAIL");
}

void WebUi::handleSeek(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("ms");
  if (raw.isEmpty() || raw.length() > 10) { res.sendText(400, "invalid seek"); return; }
  for (size_t i = 0; i < raw.length(); ++i)
    if (raw[i] < '0' || raw[i] > '9') { res.sendText(400, "invalid seek"); return; }
  const uint32_t ms = raw.toInt();
  const bool ok = audio.seekPlaybackMs(ms);
  res.sendText(ok ? 200 : 409, ok ? "OK" : "SEEK UNSUPPORTED");
}

void WebUi::handleQueue(HttpdRequest& req, HttpdResponse& res) {
  const String path = req.arg("path");
  const bool ok = audio.enqueueFile(path);
  res.sendText(ok ? 200 : 400, ok ? "QUEUED" : "QUEUE FAILED");
}

void WebUi::handleQueueClear(HttpdRequest& /*req*/, HttpdResponse& res) {
  audio.clearQueue();
  res.sendText(200, "OK");
}

void WebUi::handleRecordPause(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") { res.sendText(400, "invalid record pause"); return; }
  const bool ok = audio.pauseRecording(raw == "1");
  res.sendText(ok ? 200 : 409, ok ? "OK" : "FAIL");
}

void WebUi::handleRecordSplit(HttpdRequest& /*req*/, HttpdResponse& res) {
  const bool ok = audio.splitRecording();
  res.sendText(ok ? 200 : 409, ok ? "SPLIT" : "FAIL");
}

void WebUi::handleVox(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") { res.sendText(400, "invalid vox"); return; }
  float threshold = Config::VOX_THRESHOLD;
  uint32_t hang = Config::VOX_HANG_MS;
  if (req.hasArg("threshold")) {
    char* end = nullptr;
    threshold = strtof(req.arg("threshold").c_str(), &end);
    if (!end || *end != '\0' || !isfinite(threshold) || threshold < 0.01f || threshold > 1.0f) {
      res.sendText(400, "invalid vox threshold"); return;
    }
  }
  if (req.hasArg("hang")) {
    const String rawHang = req.arg("hang");
    if (rawHang.isEmpty() || rawHang.length() > 5) {
      res.sendText(400, "invalid vox hang"); return;
    }
    hang = static_cast<uint32_t>(rawHang.toInt());
    if (hang < 50 || hang > 5000) {
      res.sendText(400, "invalid vox hang"); return;
    }
  }
  const bool ok = audio.setVox(raw == "1", threshold, hang);
  res.sendText(ok ? 200 : 400, ok ? "OK" : "FAIL");
}

void WebUi::handleRecordQuality(HttpdRequest& req, HttpdResponse& res) {
  const String level = req.arg("level");
  uint8_t value = 2;
  if (level == "low") value = 0;
  else if (level == "medium") value = 1;
  else if (level == "high") value = 2;
  else {
    res.sendText(400, "invalid record quality");
    return;
  }

  RuntimeConfig candidate;
  uint32_t generation = 0;
  if (!configSnapshot(candidate, generation)) {
    res.send503("configuration busy");
    return;
  }
  const uint8_t previous = candidate.audioRecordQuality;
  if (previous == value) {
    res.sendText(200, "OK");
    return;
  }
  candidate.audioRecordQuality = value;

  if (!audio.setRecordQuality(value)) {
    res.sendText(409, "recording active or hardware rejected quality");
    return;
  }
  if (!configCommit(candidate, generation)) {
    (void)audio.setRecordQuality(previous);
    res.sendText(409, "configuration changed; retry");
    return;
  }
  res.sendText(200, "OK");
}

void WebUi::handleVad(HttpdRequest& req, HttpdResponse& res) {
  const String on = req.arg("on");
  if (on != "0" && on != "1") {
    res.sendText(400, "invalid vad"); return;
  }
  uint32_t adapt = 0;
  if (req.hasArg("adapt")) {
    char* end = nullptr;
    const unsigned long v = strtoul(req.arg("adapt").c_str(), &end, 10);
    if (!end || *end != '\0' || v > 60000UL) {
      res.sendText(400, "invalid adapt"); return;
    }
    adapt = static_cast<uint32_t>(v);
  }
  const bool ok = audio.setVox(on == "1",
                               req.hasArg("threshold") ? req.arg("threshold").toFloat() : 0.08f,
                               req.hasArg("hang") ? req.arg("hang").toInt() : 700);
  if (ok && adapt) (void)audio.setVoxAdapt(adapt);
  res.sendText(ok ? 200 : 400, ok ? "OK" : "invalid vox");
}

void WebUi::handleUsbTransport(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("on");
  if (raw != "0" && raw != "1") { res.sendText(400, "invalid transport"); return; }
  const bool ok = audio.setUsbPlaybackTransport(raw == "1");
  res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
}

void WebUi::handleVolume(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("value");
  if (raw.isEmpty() || raw.length() > 3) {
    res.sendText(400, "invalid volume");
    return;
  }
  for (size_t i = 0; i < raw.length(); ++i) {
    if (raw[i] < '0' || raw[i] > '9') {
      res.sendText(400, "invalid volume");
      return;
    }
  }
  const int value = raw.toInt();
  if (value < 0 || value > 100) {
    res.sendText(400, "invalid volume");
    return;
  }
  audio.setVolume(static_cast<uint8_t>(value));
  res.sendText(200, "OK");
}

void WebUi::handleDelete(HttpdRequest& req, HttpdResponse& res) {
  String p = req.arg("path");
  bool ok = storage.removeFile(p);
  res.sendText(ok ? 200 : 400, ok ? "OK" : "FAIL");
}

void WebUi::handleAudioSource(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("source");
  if (raw != "0" && raw != "1" && raw != "2" && raw != "3") {
    res.sendText(400, "invalid audio source");
    return;
  }

  const uint8_t source = static_cast<uint8_t>(raw.toInt());
  RuntimeConfig previous{};
  if (!configSnapshot(previous)) { res.send503("configuration snapshot unavailable"); return; }
  if (!audio.setRecordSource(source)) {
    res.sendText(409, "audio source cannot change while recording/playback is active");
    return;
  }
  RuntimeConfig candidate = previous; candidate.audioRecordSource = source;
  if (!configCommit(candidate)) {
    (void)audio.setRecordSource(previous.audioRecordSource);
    res.send503("audio source NVS save failed");
    return;
  }
  res.sendText(200, "OK");
}

// ---------------------------------------------------------------------------
// Helpers: sensor id, BLE address
// ---------------------------------------------------------------------------
bool WebUi::parseSensorNodeId(HttpdRequest& req, size_t& id) {
  const String raw = req.arg("id");
  if (raw.isEmpty()) return false;
  const long value = raw.toInt();
  if (value < 0 || value >= static_cast<long>(SensorRegistry::MAX_SUPPORTED_NODES)) return false;
  id = static_cast<size_t>(value);
  return true;
}

bool WebUi::parseBleAddressArg(HttpdRequest& req, SensorProtocol::BleAddress& out) {
  const String raw = req.arg("addr");
  if (raw.length() != 17) return false;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < 6; ++i) {
    const size_t pos = (5U - i) * 3U;
    if (i < 5 && raw[pos + 2] != ':') return false;
    const int hi = hex(raw[pos]), lo = hex(raw[pos + 1]);
    if (hi < 0 || lo < 0) return false;
    out.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  out.type = 0;
  return true;
}

// ---------------------------------------------------------------------------
// Handlers: track / GPS
// ---------------------------------------------------------------------------
void WebUi::handleTrack(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.sendEmpty(503); return; }
  String j = "{\"valid\":" + String(gState.gps.valid ? "true":"false") +
             ",\"lat\":" + String(gState.gps.lat,6) +
             ",\"lon\":" + String(gState.gps.lon,6) + "}";
  res.sendJson(200, j);
}

void WebUi::handleTrackPoints(HttpdRequest& req, HttpdResponse& res) {
  uint64_t from = 0, to = UINT64_MAX;
  if (req.hasArg("from")) {
    const String raw = req.arg("from");
    if (raw.isEmpty() || raw.length() > 20) {
      res.sendText(400, "invalid from"); return;
    }
    char* end = nullptr;
    from = strtoull(raw.c_str(), &end, 10);
    if (!end || *end != '\0') {
      res.sendText(400, "invalid from"); return;
    }
  }
  if (req.hasArg("to")) {
    const String raw = req.arg("to");
    if (raw.isEmpty() || raw.length() > 20) {
      res.sendText(400, "invalid to"); return;
    }
    char* end = nullptr;
    to = strtoull(raw.c_str(), &end, 10);
    if (!end || *end != '\0') {
      res.sendText(400, "invalid to"); return;
    }
  }
  size_t limit = 1000;
  if (req.hasArg("limit")) {
    const String raw = req.arg("limit");
    if (raw.isEmpty() || raw.length() > 4) {
      res.sendText(400, "invalid limit"); return;
    }
    char* end = nullptr;
    const unsigned long v = strtoul(raw.c_str(), &end, 10);
    if (!end || *end != '\0' || v == 0) {
      res.sendText(400, "invalid limit"); return;
    }
    limit = min<unsigned long>(v, 5000UL);
  }
  const String j = storage.readTrackCsv(from, to, limit);
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleTrackSimplified(HttpdRequest& req, HttpdResponse& res) {
  double epsilon = 10.0;
  if (req.hasArg("epsilon")) {
    const String raw = req.arg("epsilon");
    if (raw.isEmpty() || raw.length() > 12) {
      res.sendText(400, "invalid epsilon"); return;
    }
    char* end = nullptr;
    epsilon = strtod(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(epsilon) || epsilon <= 0.0 || epsilon > 10000.0) {
      res.sendText(400, "invalid epsilon"); return;
    }
  }
  const String j = storage.readTrackCsvSimplified(0, UINT64_MAX, 5000, epsilon);
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleTrackDownload(HttpdRequest& /*req*/, HttpdResponse& res) {
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { res.send503("busy"); return; }
  File f = SD.open("/TRACK/TRACK.CSV", FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    res.send404("track not found");
    return;
  }
  res.setHeader("Content-Disposition", "attachment; filename=TRACK.CSV");
  (void)res.streamFile(f, "text/csv");
  f.close();
}

// ---------------------------------------------------------------------------
// Handlers: BLE sensors
// ---------------------------------------------------------------------------
void WebUi::handleSensorNodes(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorNodesMs_, config.webAuthRateLimitMs)) return;
  size_t count = 0;
  if (!bleSensorReader.sensorReader().snapshotNodes(gSensorSnapshots,
                                                    SensorRegistry::MAX_SUPPORTED_NODES,
                                                    count)) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"sensor snapshot unavailable\"}");
    return;
  }
  String j = "{\"ok\":true,\"sensorDropped\":" +
             String(bleSensorReader.sensorReader().droppedSamples()) +
             ",\"queueDepth\":" + String(bleSensorReader.sensorReader().queueDepth()) +
             ",\"nodes\":[";
  for (size_t i = 0; i < count; ++i) {
    if (i) j += ',';
    j += sensorNodeJson(gSensorSnapshots[i].index, gSensorSnapshots[i].node, false);
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j + "]}");
}

void WebUi::handleSensorNodeDetail(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorNodesMs_, config.webAuthRateLimitMs)) return;
  const String raw = req.arg("id");
  if (raw.isEmpty()) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"missing id\"}");
    return;
  }
  const long id = raw.toInt();
  if (id < 0 || id >= static_cast<long>(SensorRegistry::MAX_SUPPORTED_NODES)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  SensorRegistry::Node node{};
  if (!bleSensorReader.sensorReader().snapshotNode(static_cast<size_t>(id), node)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"node not found\"}");
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, "{\"ok\":true," +
                    sensorNodeJson(static_cast<size_t>(id), node, true).substring(1));
}

void WebUi::handleSensorLive(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorLiveMs_, config.webAuthRateLimitMs)) return;
  size_t count = 0;
  if (!bleSensorReader.sensorReader().snapshotNodes(gSensorSnapshots,
                                                    SensorRegistry::MAX_SUPPORTED_NODES,
                                                    count)) {
    res.sendJson(503, "{\"ok\":false}");
    return;
  }
  String j = "{\"ok\":true,\"sensorDropped\":" +
             String(bleSensorReader.sensorReader().droppedSamples()) +
             ",\"queueDepth\":" + String(bleSensorReader.sensorReader().queueDepth()) +
             ",\"nodes\":[";
  for (size_t i = 0; i < count; ++i) {
    if (i) j += ',';
    j += sensorNodeJson(gSensorSnapshots[i].index, gSensorSnapshots[i].node, true);
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j + "]}");
}

void WebUi::handleSensorForget(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorActionMs_, config.webAuthRateLimitMs)) return;
  size_t id = 0;
  if (!parseSensorNodeId(req, id)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  if (!bleSensorReader.sensorReader().requestForgetNode(id)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"node not found\"}");
    return;
  }
  res.sendJson(202, "{\"ok\":true,\"queued\":true}");
}

void WebUi::handleSensorRefresh(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorActionMs_, config.webAuthRateLimitMs)) return;
  size_t id = 0;
  if (!parseSensorNodeId(req, id)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  if (!bleSensorReader.sensorReader().requestRefreshNode(id)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"node not found\"}");
    return;
  }
  res.sendJson(202, "{\"ok\":true,\"queued\":true}");
}

void WebUi::handleSensorQueuePolicy(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorQueuePolicyMs_, config.webAuthRateLimitMs)) return;
  const String policy = req.arg("policy");
  if (policy == "oldest")
    bleSensorReader.sensorReader().setQueuePolicy(SensorReader::SampleQueuePolicy::DROP_OLDEST);
  else if (policy == "newest")
    bleSensorReader.sensorReader().setQueuePolicy(SensorReader::SampleQueuePolicy::DROP_NEWEST);
  else {
    res.sendJson(400, "{\"ok\":false,\"error\":\"policy must be newest|oldest\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true,\"policy\":\"" + policy + "\"}");
}

void WebUi::handleSensorDedupStats(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"state unavailable\"}");
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200,
               "{\"ok\":true,\"persistenceFailures\":" +
                   String(gState.sensorDedupFailures) +
                   ",\"sensorBatchAckRetries\":" + String(gState.sensorBatchAckRetries) +
                   ",\"sensorBatchAckFailures\":" + String(gState.sensorBatchAckFailures) +
                   ",\"remoteSensorOverflowDrops\":" + String(gState.remoteSensorOverflowDrops) +
                   ",\"remoteSensorLegacyAccepted\":" + String(gState.remoteSensorLegacyAccepted) +
                   ",\"remoteSensorLegacyRejected\":" + String(gState.remoteSensorLegacyRejected) + "}");
}

void WebUi::handleSensorSpool(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorNodesMs_, config.webAuthRateLimitMs)) return;
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, sensorSpool.statusJson());
}

void WebUi::handleSensorSpoolClear(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorActionMs_, config.webAuthRateLimitMs)) return;
  if (!sensorSpool.clear()) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"spool clear failed\"}");
    return;
  }
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.sensorSpoolDepth = 0;
      gState.sensorSpoolEvictions = sensorSpool.evictions();
      gState.sensorSpoolDrops = sensorSpool.drops();
      gState.sensorSpoolRecovered = sensorSpool.recovered();
    }
  }
  res.sendJson(200, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Handlers: BLE passkey
// ---------------------------------------------------------------------------
void WebUi::handleBlePasskeySet(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastBlePasskeyMs_, config.webAuthRateLimitMs)) return;
  SensorProtocol::BleAddress address{};
  const String pass = req.arg("passkey");
  if (!parseBleAddressArg(req, address) || pass.length() != 6) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid addr/passkey\"}");
    return;
  }
  for (size_t i = 0; i < 6; ++i) {
    if (!isdigit(static_cast<unsigned char>(pass[i]))) {
      res.sendJson(400, "{\"ok\":false,\"error\":\"passkey must be 6 digits\"}");
      return;
    }
  }
  const uint32_t value = static_cast<uint32_t>(pass.toInt());
  if (value < 100000U || value > 999999U ||
      !bleSensorReader.setPeerPasskey(address, value)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"passkey rejected\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true}");
}

void WebUi::handleBlePasskeyDelete(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastBlePasskeyMs_, config.webAuthRateLimitMs)) return;
  SensorProtocol::BleAddress address{};
  if (!parseBleAddressArg(req, address) ||
      !bleSensorReader.forgetPeerPasskey(address)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"peer not found\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true}");
}

void WebUi::handleBlePasskeyList(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastBlePasskeyMs_, config.webAuthRateLimitMs)) return;
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, "{\"ok\":true,\"peers\":" + bleSensorReader.peersJson() + "}");
}

// ---------------------------------------------------------------------------
// Handlers: MQTT
// ---------------------------------------------------------------------------
void WebUi::handleMqttProvision(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  static uint32_t lastMqttProvisionMs = 0;
  if (!rateLimit(req, res, lastMqttProvisionMs, config.webAuthRateLimitMs)) return;
  const String host = req.arg("host");
  const String rawPort = req.arg("port");
  const String certificatePem = req.arg("cert");
  const String privateKeyPem = req.arg("key");
  if (host.isEmpty() || host.length() > 253 || rawPort.isEmpty() ||
      rawPort.length() > 5 || certificatePem.length() < 64 ||
      certificatePem.length() > 8192 || privateKeyPem.length() < 64 ||
      privateKeyPem.length() > 8192) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid MQTT PKI material\"}");
    return;
  }
  uint32_t port = 0;
  for (size_t i = 0; i < rawPort.length(); ++i) {
    if (rawPort[i] < '0' || rawPort[i] > '9') {
      res.sendJson(400, "{\"ok\":false,\"error\":\"invalid MQTT port\"}");
      return;
    }
    port = port * 10U + static_cast<uint32_t>(rawPort[i] - '0');
  }
  if (port == 0 || port > 65535U || host.indexOf('|') >= 0 ||
      certificatePem.indexOf("-----BEGIN CERTIFICATE-----") < 0 ||
      certificatePem.indexOf("-----END CERTIFICATE-----") < 0 ||
      privateKeyPem.indexOf("-----BEGIN") < 0 ||
      privateKeyPem.indexOf("PRIVATE KEY-----") < 0) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid MQTT PKI material\"}");
    return;
  }
  if (!mqtt.provisionCertificate(host, static_cast<uint16_t>(port),
                                 certificatePem, privateKeyPem)) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"MQTT PKI provisioning failed\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true,\"provisioned\":true,\"auth\":\"x509\"}");
}

void WebUi::handleMqttStatus(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  static uint32_t lastMqttStatusMs = 0;
  if (!rateLimit(req, res, lastMqttStatusMs, config.webAuthRateLimitMs)) return;
  String j = "{\"ok\":true,\"provisioned\":";
  j += mqtt.credentialsProvisioned() ? "true" : "false";
  j += ",\"auth\":\"x509\"";
  j += ",\"connected\":";
  j += mqtt.isConnected() ? "true" : "false";
  j += ",\"passwordRotationWarning\":";
  j += mqtt.passwordRotationWarning() ? "true" : "false";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleMqttCertStatus(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, certLifecycle.statusJson());
}

void WebUi::handleMqttCertRenew(HttpdRequest& req, HttpdResponse& res) {
  static uint32_t lastRenewMs = 0;
  if (!rateLimit(req, res, lastRenewMs, 60000UL)) return;
  const bool ok = certLifecycle.renewCertificate(true);
  res.sendJson(ok ? 200 : 503,
               ok ? "{\"ok\":true,\"status\":\"renewed\"}"
                  : "{\"ok\":false,\"status\":\"renew_failed\"}");
}

void WebUi::handleMqttCertHistory(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, certLifecycle.historyJson());
}

void WebUi::handleMqttCertCaChain(HttpdRequest& req, HttpdResponse& res) {
  static uint32_t lastCaFetchMs = 0;
  if (!rateLimit(req, res, lastCaFetchMs, 60000UL)) return;
  const bool ok = certLifecycle.fetchCaChain();
  res.sendJson(ok ? 200 : 503,
               ok ? "{\"ok\":true,\"status\":\"fetched\"}"
                  : "{\"ok\":false,\"status\":\"fetch_failed\"}");
}

// ---------------------------------------------------------------------------
// Handlers: config / factory reset / reboot
// ---------------------------------------------------------------------------
void WebUi::handleConfigExport(HttpdRequest& /*req*/, HttpdResponse& res) {
  RuntimeConfig c;
  if (!configSnapshot(c)) {
    res.send503("configuration unavailable");
    return;
  }
  String j = "{";
  j += "\"freq\":" + String(c.loraFreqMHz, 3);
  j += ",\"bw\":" + String(c.loraBwKHz, 3);
  j += ",\"sf\":" + String(c.loraSf) + ",\"cr\":" + String(c.loraCr);
  j += ",\"sync\":" + String(c.loraSyncWord) + ",\"power\":" + String(c.loraPowerDbm);
  j += ",\"volume\":" + String(c.volume) + ",\"audio_source\":" + String(c.audioRecordSource);
  j += ",\"record_quality\":" + String(c.audioRecordQuality);
  j += ",\"ble_pairing\":" + String(c.blePairingEnabled ? "true" : "false");
  j += ",\"battery_calibration\":" + String(c.batteryCalibration, 5);
  j += ",\"mqtt_enabled\":" + String(c.mqttEnabled ? "true" : "false");
  j += ",\"mqtt_host\":\"" + jsonEscape(c.mqttHost) + "\"";
  j += ",\"mqtt_port\":" + String(c.mqttPort);
  j += ",\"mqtt_tls\":" + String(c.mqttTlsRequired ? "true" : "false");
  j += ",\"mqtt_reconnect_min_ms\":" + String(c.mqttReconnectMinMs);
  j += ",\"mqtt_reconnect_max_ms\":" + String(c.mqttReconnectMaxMs);
  j += ",\"mqtt_telemetry_period_ms\":" + String(c.mqttTelemetryPeriodMs);
  j += ",\"mqtt_health_period_ms\":" + String(c.mqttHealthPeriodMs);
  j += ",\"mqtt_retain_telemetry\":" + String(c.mqttRetainTelemetry ? "true" : "false");
  j += ",\"mqtt_retain_availability\":" + String(c.mqttRetainAvailability ? "true" : "false");
  j += ",\"mqtt_rotation_days\":" + String(c.mqttCredentialRotationDays);
  j += ",\"vox_enabled\":" + String(c.voxEnabled ? "true" : "false");
  j += ",\"vox_threshold\":" + String(c.voxThreshold, 4);
  j += ",\"vox_hang_ms\":" + String(c.voxHangMs);
  j += ",\"aec_enabled\":" + String(c.aecEnabled ? "true" : "false");
  j += ",\"usb_monitor\":" + String(c.usbMonitor ? "true" : "false");
  j += ",\"usb_transport\":" + String(c.usbPlaybackTransport ? "true" : "false");
  j += ",\"loopback\":" + String(c.audioLoopback ? "true" : "false");
  j += ",\"adr_enabled\":" + String(c.loraAdrEnabled ? "true" : "false");
  j += ",\"hop_enabled\":" + String(c.loraHopEnabled ? "true" : "false");
  j += ",\"hop_profile\":" + String(c.loraHopChannelProfile);
  j += ",\"range_test_mode\":" + String(c.loraRangeTestMode ? "true" : "false");
  j += ",\"ble_enabled\":" + String(c.sensorReaderEnabled ? "true" : "false");
  j += ",\"ble_scan_interval_ms\":" + String(c.sensorScanIntervalMs);
  j += ",\"ble_scan_window_ms\":" + String(c.sensorScanWindowMs);
  j += ",\"ble_scan_duration_ms\":" + String(c.sensorScanDurationMs);
  j += ",\"ble_connect_timeout_ms\":" + String(c.sensorConnectTimeoutMs);
  j += ",\"ble_eviction_ms\":" + String(c.sensorNodeEvictionMs);
  j += ",\"ble_max_nodes\":" + String(c.sensorMaxNodes);
  j += ",\"ble_encryption\":" + String(c.sensorRequireEncryption ? "true" : "false");
  j += ",\"ble_failure_threshold\":" + String(c.blePairingFailureThreshold);
  j += ",\"ble_block_ms\":" + String(c.blePairingBlockMs);
  j += ",\"ble_keep_awake\":" + String(c.sensorKeepAwake ? "true" : "false");
  j += ",\"web_session_timeout_ms\":" + String(c.webSessionTimeoutMs);
  j += ",\"web_auth_rate_limit_ms\":" + String(c.webAuthRateLimitMs);
  j += ",\"csrf_policy\":" + String(c.csrfPolicy);
  j += ",\"ble_pairing_policy\":" + String(c.blePairingPolicy);
  j += ",\"ecdh_rekey_policy\":" + String(c.ecdhRekeyPolicy);
  j += ",\"replay_window_bits\":" + String(c.replayWindowBits);
  j += ",\"est_server_url\":\"" + jsonEscape(c.estServerUrl) + "\"";
  j += ",\"est_label\":\"" + jsonEscape(c.estLabel) + "\"";
  j += ",\"est_auth_mode\":" + String(c.estAuthMode);
  j += ",\"cert_renewal_threshold_days\":" + String(c.certRenewalThresholdDays);
  j += ",\"cert_check_period_ms\":" + String(c.certCheckPeriodMs);
  j += ",\"cert_lifecycle_enabled\":" + String(c.certLifecycleEnabled ? "true" : "false");
  j += ",\"wake_period_sec\":" + String(c.wakePeriodSec);
  j += ",\"deep_sleep_enabled\":" + String(c.deepSleepEnabled ? "true" : "false");
  j += ",\"deep_sleep_idle_sec\":" + String(c.deepSleepIdleMs / 1000UL);
  j += ",\"deep_sleep_wake_grace_ms\":" + String(c.deepSleepWakeGraceMs);
  j += ",\"critical_shutdown_delay_ms\":" + String(c.criticalShutdownDelayMs);
  j += ",\"battery_low_threshold\":" + String(c.batteryLowThreshold, 3);
  j += ",\"battery_critical_threshold\":" + String(c.batteryCriticalThreshold, 3);
  j += ",\"classd_enabled\":" + String(c.classDEnabled ? "true" : "false");
  j += ",\"classd_boost\":" + String(c.classDBoostLevel);
  j += ",\"callsign\":\"" + jsonEscape(c.callsign) + "\"";
  j += "}";
  res.setHeader("Content-Disposition",
                "attachment; filename=\"fieldradio-config.json\"");
  res.sendJson(200, j);
}

void WebUi::handleConfigBackup(HttpdRequest& /*req*/, HttpdResponse& res) {
  const String envelope = encryptConfigBackup();
  if (envelope.isEmpty()) {
    res.send503("backup unavailable");
    return;
  }
  res.setHeader("Content-Disposition",
                "attachment; filename=\"fieldradio-config.frb\"");
  res.sendText(200, envelope);
}

void WebUi::handleConfigRestore(HttpdRequest& req, HttpdResponse& res) {
  String body;
  if (!req.bodyText(body, 4096)) {
    res.sendText(413, "backup too large"); return;
  }
  if (body.length() < 16) {
    res.sendText(400, "invalid backup");
    return;
  }
  String plain;
  if (!decryptConfigBackup(body, plain)) {
    res.sendText(400, "backup authentication failed");
    return;
  }

  RuntimeConfig candidate{};
  if (!configSnapshot(candidate)) { res.send503("configuration snapshot unavailable"); return; }
  bool seenFreq = false, seenBw = false, seenSf = false, seenCr = false;
  int pos = 0;
  while (pos <= static_cast<int>(plain.length())) {
    const int nl = plain.indexOf('\n', pos);
    const int end = nl < 0 ? plain.length() : nl;
    const String line = plain.substring(pos, end);
    String key, value;
    if (!line.isEmpty()) {
      if (!parseBackupLine(line, key, value)) {
        res.sendText(400, "malformed backup");
        return;
      }
      if (key == "freq") { candidate.loraFreqMHz = value.toFloat(); seenFreq = true; }
      else if (key == "bw") { candidate.loraBwKHz = value.toFloat(); seenBw = true; }
      else if (key == "sf") { candidate.loraSf = static_cast<uint8_t>(value.toInt()); seenSf = true; }
      else if (key == "cr") { candidate.loraCr = static_cast<uint8_t>(value.toInt()); seenCr = true; }
      else if (key == "sync") candidate.loraSyncWord = static_cast<uint8_t>(value.toInt());
      else if (key == "power") candidate.loraPowerDbm = static_cast<int8_t>(value.toInt());
      else if (key == "volume") candidate.volume = static_cast<uint8_t>(value.toInt());
      else if (key == "audsrc") candidate.audioRecordSource = static_cast<uint8_t>(value.toInt());
      else if (key == "recqual") candidate.audioRecordQuality = static_cast<uint8_t>(value.toInt());
      else if (key == "batcal") candidate.batteryCalibration = value.toFloat();
      else if (key == "callsign") candidate.callsign = value;
      else if (key == "lorakey") candidate.loraKeyHex = value;
      else if (key == "apssid") candidate.apSsid = value;
      else if (key == "apppass") candidate.apPassword = value;
      else if (key == "stassid") candidate.staSsid = value;
      else if (key == "stapass") candidate.staPassword = value;
      else if (key == "webuser") candidate.webUser = value;
      else if (key == "websalt") candidate.webPasswordSaltHex = value;
      else if (key == "webph") candidate.webPasswordHashHex = value;
      else if (key == "mqtt_en") candidate.mqttEnabled = value == "1";
      else if (key == "wake_sec") candidate.wakePeriodSec = static_cast<uint32_t>(value.toInt());
      else if (key == "sleep_en") candidate.deepSleepEnabled = value == "1";
      else if (key == "sleep_idle") candidate.deepSleepIdleMs = static_cast<uint32_t>(value.toInt());
      else if (key == "wake_grace") candidate.deepSleepWakeGraceMs = static_cast<uint32_t>(value.toInt());
      else if (key == "bat_crit_delay") candidate.criticalShutdownDelayMs = static_cast<uint32_t>(value.toInt());
      else if (key == "bat_low") candidate.batteryLowThreshold = value.toFloat();
      else if (key == "bat_critical") candidate.batteryCriticalThreshold = value.toFloat();
      else if (key == "classd_en") candidate.classDEnabled = value == "1";
      else if (key == "classd_boost") candidate.classDBoostLevel = static_cast<uint8_t>(value.toInt());
      else { res.sendText(400, "unknown backup key"); return; }
    }
    if (nl < 0) break;
    pos = nl + 1;
  }
  if (!seenFreq || !seenBw || !seenSf || !seenCr || !candidate.validSemantics() ||
      candidate.volume > 100 || candidate.audioRecordQuality > 2 ||
      candidate.audioRecordSource > Config::AUDIO_SOURCE_USB ||
      candidate.wakePeriodSec < Config::WAKE_PERIOD_SEC_MIN || candidate.wakePeriodSec > Config::WAKE_PERIOD_SEC_MAX ||
      candidate.deepSleepIdleMs < 60000UL ||
      candidate.deepSleepIdleMs > 86400000UL ||
      candidate.deepSleepWakeGraceMs < 100UL ||
      candidate.deepSleepWakeGraceMs > 60000UL ||
      candidate.criticalShutdownDelayMs < 100UL ||
      candidate.criticalShutdownDelayMs > 600000UL ||
      !isfinite(candidate.batteryLowThreshold) ||
      !isfinite(candidate.batteryCriticalThreshold) ||
      candidate.batteryCriticalThreshold < Config::BATTERY_CRITICAL_THRESHOLD_MIN ||
      candidate.batteryLowThreshold <= candidate.batteryCriticalThreshold ||
      candidate.batteryLowThreshold > Config::BATTERY_LOW_THRESHOLD_MAX ||
      candidate.classDBoostLevel > 7 ||
      candidate.staSsid.length() > Config::STA_SSID_MAX_LEN ||
      ((!candidate.staSsid.isEmpty()) &&
       (candidate.staPassword.length() < Config::STA_PASSWORD_MIN_LEN ||
        candidate.staPassword.length() > Config::STA_PASSWORD_MAX_LEN)) ||
      (candidate.classDEnabled && !Config::CLASS_D_ENABLED) ||
      !candidate.webPasswordConfigured()) {
    res.sendText(400, "backup config invalid");
    return;
  }

  RuntimeConfig previous{};
  if (!configSnapshot(previous)) { res.send503("configuration snapshot unavailable"); return; }
  const uint8_t previousSource = audio.recordSource();
  if (!configCommit(candidate)) { res.send503("NVS restore failed"); return; }
  auto rollback = [&]() {
    (void)configCommit(previous); (void)lora.applyConfig();
    (void)audio.setClassDConfig(previous.classDEnabled,previous.classDBoostLevel);
    (void)audio.setVox(previous.voxEnabled,previous.voxThreshold,previous.voxHangMs);
    (void)audio.setAec(previous.aecEnabled); (void)audio.setUsbMonitor(previous.usbMonitor);
    (void)audio.setUsbPlaybackTransport(previous.usbPlaybackTransport); (void)audio.setLoopback(previous.audioLoopback);
    (void)audio.applyRecordQualityRuntime(previous.audioRecordQuality); (void)audio.setRecordSource(previousSource);
    mqtt.setEnabled(previous.mqttEnabled); audio.setVolume(previous.volume);
  };
  if (!audio.setClassDConfig(candidate.classDEnabled,candidate.classDBoostLevel) ||
      !audio.setVox(candidate.voxEnabled,candidate.voxThreshold,candidate.voxHangMs) ||
      !audio.setAec(candidate.aecEnabled) || !audio.setUsbMonitor(candidate.usbMonitor) ||
      !audio.setUsbPlaybackTransport(candidate.usbPlaybackTransport) || !audio.setLoopback(candidate.audioLoopback) ||
      !audio.applyRecordQualityRuntime(candidate.audioRecordQuality)) {
    rollback(); res.send503("audio restore failed"); return;
  }
  mqtt.setEnabled(candidate.mqttEnabled);
  if (!lora.applyConfig()) { rollback(); res.send503("radio restore failed"); return; }
  if (!audio.setRecordSource(candidate.audioRecordSource)) { rollback(); res.send503("audio source restore failed"); return; }
  audio.setVolume(candidate.volume);

  lora.updateSourceId();
  res.sendText(200, "OK; reboot recommended");
}

void WebUi::handleFactoryReset(HttpdRequest& req, HttpdResponse& res) {
  if (req.arg("confirm") != "RESET") {
    res.sendText(400, "confirmation required");
    return;
  }
  if (!lora.prepareForFactoryReset()) {
    res.send503("LoRa storage busy");
    return;
  }
  {
    SpiLock spiLock(pdMS_TO_TICKS(500));
    if (!spiLock.ok()) {
      res.send503("storage busy");
      return;
    }
    static const char* const managedDirs[] = {"/REC", "/LOG", "/TRACK", "/LORA"};
    for (const char* dir : managedDirs) {
      if (SD.exists(dir) && !eraseStorageTree(dir)) {
        lora.cancelFactoryReset();
        res.send503("SD data erase failed");
        return;
      }
    }
  }

  const esp_err_t err = nvs_flash_erase();
  if (err != ESP_OK) {
    lora.cancelFactoryReset();
    res.send503("NVS secure erase failed");
    return;
  }
  res.sendText(200, "factory reset; rebooting");
  delay(100);
  ESP.restart();
}

void WebUi::handleReboot(HttpdRequest& /*req*/, HttpdResponse& res) {
  (void)sensorSpool.flush();
  res.sendText(200, "rebooting");
  delay(100);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Handlers: config (POST /api/config) — the last remaining handler
// ---------------------------------------------------------------------------
void WebUi::handleConfig(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig candidate{};
  uint32_t configGenerationSnapshot = 0;
  if (!configSnapshot(candidate, configGenerationSnapshot)) {
    res.send503("configuration snapshot unavailable");
    return;
  }
  if (!rateLimit(req, res, lastConfigMs_, candidate.webAuthRateLimitMs)) return;
  bool radioChanged = false;

  auto parseUnsigned = [](const String& raw, uint32_t maxValue, uint32_t& out) {
    return WebUiNumericParser::parseUnsigned(raw.c_str(), raw.length(), maxValue, out);
  };

  auto validHex32 = [](const String& raw) {
    if (raw.length() != 32) return false;
    for (size_t i = 0; i < raw.length(); ++i) {
      const char c = raw[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')))
        return false;
    }
    return true;
  };
  auto validPassword = [](const String& raw) {
    if (raw.length() < 8 || raw.length() > 63) return false;
    for (size_t i = 0; i < raw.length(); ++i) {
      const uint8_t c = static_cast<uint8_t>(raw[i]);
      if (c < 0x20 || c == 0x7F || c == '\\' || c == '"') return false;
    }
    return true;
  };
  auto parseBoolArg = [&](const char* name, bool& out) {
    if (!req.hasArg(name)) return true;
    const String raw = req.arg(name);
    if (raw == "0") { out = false; return true; }
    if (raw == "1") { out = true;  return true; }
    return false;
  };

  // ---- Radio ----
  if (req.hasArg("freq")) {
    const String raw = req.arg("freq");
    char* end = nullptr;
    const float value = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(value)) {
      res.sendText(400, "invalid frequency"); return;
    }
    candidate.loraFreqMHz = value;
    radioChanged = true;
  }
  if (req.hasArg("bw")) {
    const String raw = req.arg("bw");
    char* end = nullptr;
    const float value = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(value)) {
      res.sendText(400, "invalid bandwidth"); return;
    }
    candidate.loraBwKHz = value;
    radioChanged = true;
  }

  uint32_t value = 0;
  if (req.hasArg("sf")) {
    if (!parseUnsigned(req.arg("sf"), 12, value) || value < 5) {
      res.sendText(400, "invalid spreading factor"); return;
    }
    candidate.loraSf = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (req.hasArg("cr")) {
    if (!parseUnsigned(req.arg("cr"), 8, value) || value < 5) {
      res.sendText(400, "invalid coding rate"); return;
    }
    candidate.loraCr = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (req.hasArg("sync")) {
    if (!parseUnsigned(req.arg("sync"), 255, value)) {
      res.sendText(400, "invalid sync word"); return;
    }
    candidate.loraSyncWord = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (req.hasArg("power")) {
    if (!parseUnsigned(req.arg("power"), 17, value) || value < 2) {
      res.sendText(400, "invalid power"); return;
    }
    candidate.loraPowerDbm = static_cast<int8_t>(value);
    radioChanged = true;
  }

  // ---- Audio ----
  if (req.hasArg("volume")) {
    if (!parseUnsigned(req.arg("volume"), 100, value)) {
      res.sendText(400, "invalid volume"); return;
    }
    candidate.volume = static_cast<uint8_t>(value);
  }
  if (req.hasArg("audio_source")) {
    if (!parseUnsigned(req.arg("audio_source"), Config::AUDIO_SOURCE_USB, value)) {
      res.sendText(400, "invalid audio source"); return;
    }
    candidate.audioRecordSource = static_cast<uint8_t>(value);
  }
  if (req.hasArg("batcal")) {
    const String raw = req.arg("batcal");
    char* end = nullptr;
    const float batcal = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(batcal) ||
        batcal < 0.5f || batcal > 1.5f) {
      res.sendText(400, "invalid battery calibration"); return;
    }
    candidate.batteryCalibration = batcal;
  }

  // ---- Identity / credentials ----
  if (req.hasArg("callsign")) candidate.callsign = req.arg("callsign");
  if (req.hasArg("lora_key")) candidate.loraKeyHex = req.arg("lora_key");
  if (req.hasArg("ap_password")) candidate.apPassword = req.arg("ap_password");
  if (req.hasArg("sta_ssid")) {
    candidate.staSsid = req.arg("sta_ssid");
    if (candidate.staSsid.length() > Config::STA_SSID_MAX_LEN) {
      res.sendText(400, "invalid STA SSID"); return;
    }
    if (candidate.staSsid.isEmpty()) candidate.staPassword.clear();
  }
  if (req.hasArg("sta_password")) {
    candidate.staPassword = req.arg("sta_password");
    if (!candidate.staSsid.isEmpty() &&
        (candidate.staPassword.length() < Config::STA_PASSWORD_MIN_LEN ||
         candidate.staPassword.length() > Config::STA_PASSWORD_MAX_LEN ||
         !validPassword(candidate.staPassword))) {
      res.sendText(400, "invalid STA password"); return;
    }
  }
  if (req.hasArg("web_password")) candidate.webPassword = req.arg("web_password");

  // ---- BLE pairing ----
  if (req.hasArg("ble_pairing")) {
    const String raw = req.arg("ble_pairing");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid BLE pairing setting"); return;
    }
    candidate.blePairingEnabled = raw == "1";
  }

  // ---- MQTT ----
  if (req.hasArg("mqtt_enabled")) {
    const String raw = req.arg("mqtt_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid MQTT setting"); return;
    }
    candidate.mqttEnabled = raw == "1";
  }
  if (req.hasArg("mqtt_host")) {
    candidate.mqttHost = req.arg("mqtt_host");
    if (candidate.mqttHost.isEmpty() || candidate.mqttHost.length() > 253 ||
        candidate.mqttHost.indexOf('|') >= 0) {
      res.sendText(400, "invalid MQTT host"); return;
    }
  }
  if (req.hasArg("mqtt_port")) {
    if (!parseUnsigned(req.arg("mqtt_port"), 65535, value) || value == 0) {
      res.sendText(400, "invalid MQTT port"); return;
    }
    candidate.mqttPort = static_cast<uint16_t>(value);
  }
  if (req.hasArg("mqtt_tls")) {
    const String raw = req.arg("mqtt_tls");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid MQTT TLS setting"); return;
    }
    if (Config::mqttTlsIsMandatory() && raw == "0") {
      res.sendText(400, "MQTT TLS is mandatory in this build"); return;
    }
    candidate.mqttTlsRequired = raw == "1";
  }
  if (req.hasArg("mqtt_reconnect_min_ms")) {
    if (!parseUnsigned(req.arg("mqtt_reconnect_min_ms"),
                       Config::MQTT_RECONNECT_MS_MAX, value) ||
        value < Config::MQTT_RECONNECT_MS_MIN) {
      res.sendText(400, "invalid MQTT reconnect minimum"); return;
    }
    candidate.mqttReconnectMinMs = value;
  }
  if (req.hasArg("mqtt_reconnect_max_ms")) {
    if (!parseUnsigned(req.arg("mqtt_reconnect_max_ms"),
                       Config::MQTT_RECONNECT_MS_MAX, value) ||
        value < Config::MQTT_RECONNECT_MS_MIN) {
      res.sendText(400, "invalid MQTT reconnect maximum"); return;
    }
    candidate.mqttReconnectMaxMs = value;
  }
  if (req.hasArg("mqtt_telemetry_period_ms")) {
    if (!parseUnsigned(req.arg("mqtt_telemetry_period_ms"),
                       Config::MQTT_TELEMETRY_PERIOD_MS_MAX, value) ||
        value < Config::MQTT_TELEMETRY_PERIOD_MS_MIN) {
      res.sendText(400, "invalid MQTT telemetry period"); return;
    }
    candidate.mqttTelemetryPeriodMs = value;
  }
  if (req.hasArg("mqtt_health_period_ms")) {
    if (!parseUnsigned(req.arg("mqtt_health_period_ms"), 86400000, value) ||
        value < 1000) {
      res.sendText(400, "invalid MQTT health period"); return;
    }
    candidate.mqttHealthPeriodMs = value;
  }
  if (!parseBoolArg("mqtt_retain_telemetry", candidate.mqttRetainTelemetry) ||
      !parseBoolArg("mqtt_retain_availability", candidate.mqttRetainAvailability)) {
    res.sendText(400, "invalid MQTT retain setting"); return;
  }
  if (req.hasArg("mqtt_rotation_days")) {
    if (!parseUnsigned(req.arg("mqtt_rotation_days"), 3650, value) || value < 1) {
      res.sendText(400, "invalid MQTT rotation policy"); return;
    }
    candidate.mqttCredentialRotationDays = static_cast<uint16_t>(value);
  }

  // ---- EST / certificate lifecycle ----
  if (req.hasArg("est_server_url")) {
    candidate.estServerUrl = req.arg("est_server_url");
    if (candidate.estServerUrl.length() > 253 ||
        (!candidate.estServerUrl.isEmpty() &&
         !candidate.estServerUrl.startsWith("https://"))) {
      res.sendText(400, "invalid EST server URL"); return;
    }
  }
  if (req.hasArg("est_label")) {
    candidate.estLabel = req.arg("est_label");
    if (candidate.estLabel.isEmpty() || candidate.estLabel.length() > 95 ||
        !candidate.estLabel.startsWith("/") ||
        candidate.estLabel.indexOf('|') >= 0) {
      res.sendText(400, "invalid EST label"); return;
    }
  }
  if (req.hasArg("cert_renewal_threshold_days")) {
    if (!parseUnsigned(req.arg("cert_renewal_threshold_days"),
                       Config::CERT_RENEWAL_THRESHOLD_DAYS_MAX, value) ||
        value < Config::CERT_RENEWAL_THRESHOLD_DAYS_MIN) {
      res.sendText(400, "invalid certificate renewal threshold"); return;
    }
    candidate.certRenewalThresholdDays = static_cast<uint16_t>(value);
  }
  if (req.hasArg("cert_check_period_ms")) {
    if (!parseUnsigned(req.arg("cert_check_period_ms"), 7UL * 86400000UL, value) ||
        value < 3600000UL) {
      res.sendText(400, "invalid certificate check period"); return;
    }
    candidate.certCheckPeriodMs = value;
  }
  if (req.hasArg("est_auth_mode")) {
    if (!parseUnsigned(req.arg("est_auth_mode"), 2, value)) {
      res.sendText(400, "invalid EST auth mode"); return;
    }
    candidate.estAuthMode = static_cast<uint8_t>(value);
  }
  if (req.hasArg("est_username")) {
    candidate.estUsername = req.arg("est_username");
    if (candidate.estUsername.length() > 64) {
      res.sendText(400, "invalid EST username"); return;
    }
    for (size_t i = 0; i < candidate.estUsername.length(); ++i)
      if (static_cast<uint8_t>(candidate.estUsername[i]) < 0x20 ||
          static_cast<uint8_t>(candidate.estUsername[i]) == 0x7F) {
        res.sendText(400, "invalid EST username"); return;
      }
  }
  if (req.hasArg("est_password")) {
    candidate.estPassword = req.arg("est_password");
    if (candidate.estPassword.length() > 64) {
      res.sendText(400, "invalid EST password"); return;
    }
    for (size_t i = 0; i < candidate.estPassword.length(); ++i)
      if (static_cast<uint8_t>(candidate.estPassword[i]) < 0x20 ||
          static_cast<uint8_t>(candidate.estPassword[i]) == 0x7F) {
        res.sendText(400, "invalid EST password"); return;
      }
  }
  if (req.hasArg("est_bootstrap_token")) {
    candidate.estBootstrapToken = req.arg("est_bootstrap_token");
    if (candidate.estBootstrapToken.length() > 128) {
      res.sendText(400, "invalid EST bootstrap token"); return;
    }
    for (size_t i = 0; i < candidate.estBootstrapToken.length(); ++i)
      if (static_cast<uint8_t>(candidate.estBootstrapToken[i]) < 0x20 ||
          static_cast<uint8_t>(candidate.estBootstrapToken[i]) == 0x7F) {
        res.sendText(400, "invalid EST bootstrap token"); return;
      }
  }
  if (req.hasArg("cert_lifecycle_enabled")) {
    const String raw = req.arg("cert_lifecycle_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid certificate lifecycle setting"); return;
    }
    candidate.certLifecycleEnabled = raw == "1";
  }

  // ---- Deep sleep / wake ----
  if (req.hasArg("wake_period_sec")) {
    if (!parseUnsigned(req.arg("wake_period_sec"),
                       Config::WAKE_PERIOD_SEC_MAX, value) ||
        value < Config::WAKE_PERIOD_SEC_MIN) {
      res.sendText(400, "invalid wake period"); return;
    }
    candidate.wakePeriodSec = value;
  }
  if (req.hasArg("deep_sleep_enabled")) {
    const String raw = req.arg("deep_sleep_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid deep-sleep setting"); return;
    }
    candidate.deepSleepEnabled = raw == "1";
  }
  if (req.hasArg("deep_sleep_idle_sec")) {
    if (!parseUnsigned(req.arg("deep_sleep_idle_sec"),
                       Config::DEEP_SLEEP_IDLE_MS_MAX / 1000UL, value) ||
        value < Config::DEEP_SLEEP_IDLE_MS_MIN / 1000UL) {
      res.sendText(400, "invalid deep-sleep idle timeout"); return;
    }
    candidate.deepSleepIdleMs = value * 1000UL;
  }
  if (req.hasArg("deep_sleep_wake_grace_ms")) {
    if (!parseUnsigned(req.arg("deep_sleep_wake_grace_ms"), 60000, value) ||
        value < 100) {
      res.sendText(400, "invalid wake grace period"); return;
    }
    candidate.deepSleepWakeGraceMs = value;
  }
  if (req.hasArg("critical_shutdown_delay_ms")) {
    if (!parseUnsigned(req.arg("critical_shutdown_delay_ms"), 600000, value) ||
        value < 100) {
      res.sendText(400, "invalid critical shutdown delay"); return;
    }
    candidate.criticalShutdownDelayMs = value;
  }
  if (req.hasArg("battery_low_threshold") ||
      req.hasArg("battery_critical_threshold")) {
    const String lowRaw = req.hasArg("battery_low_threshold")
        ? req.arg("battery_low_threshold")
        : String(candidate.batteryLowThreshold, 3);
    const String criticalRaw = req.hasArg("battery_critical_threshold")
        ? req.arg("battery_critical_threshold")
        : String(candidate.batteryCriticalThreshold, 3);
    char* lowEnd = nullptr;
    char* criticalEnd = nullptr;
    const float low = strtof(lowRaw.c_str(), &lowEnd);
    const float critical = strtof(criticalRaw.c_str(), &criticalEnd);
    if (!lowEnd || *lowEnd != '\0' || !criticalEnd || *criticalEnd != '\0' ||
        !isfinite(low) || !isfinite(critical) || critical < 2.5f ||
        low <= critical || low > 4.2f) {
      res.sendText(400, "invalid battery thresholds"); return;
    }
    candidate.batteryLowThreshold = low;
    candidate.batteryCriticalThreshold = critical;
  }

  // ---- Class-D ----
  if (req.hasArg("classd_enabled")) {
    const String raw = req.arg("classd_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid Class-D setting"); return;
    }
    candidate.classDEnabled = raw == "1";
  }
  if (req.hasArg("classd_boost")) {
    if (!parseUnsigned(req.arg("classd_boost"), 7, value)) {
      res.sendText(400, "invalid Class-D boost"); return;
    }
    candidate.classDBoostLevel = static_cast<uint8_t>(value);
  }

  // ---- VOX ----
  if (!parseBoolArg("vox_enabled", candidate.voxEnabled)) {
    res.sendText(400, "invalid VOX setting"); return;
  }
  if (req.hasArg("vox_threshold")) {
    const String raw = req.arg("vox_threshold");
    char* end = nullptr;
    const float threshold = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(threshold)) {
      res.sendText(400, "invalid VOX threshold"); return;
    }
    candidate.voxThreshold = threshold;
  }
  if (req.hasArg("vox_hang_ms")) {
    if (!parseUnsigned(req.arg("vox_hang_ms"), 10000, value) || value < 50) {
      res.sendText(400, "invalid VOX hang time"); return;
    }
    candidate.voxHangMs = value;
  }

  // ---- Booleans ----
  if (!parseBoolArg("aec_enabled", candidate.aecEnabled) ||
      !parseBoolArg("usb_monitor", candidate.usbMonitor) ||
      !parseBoolArg("usb_transport", candidate.usbPlaybackTransport) ||
      !parseBoolArg("loopback", candidate.audioLoopback) ||
      !parseBoolArg("adr_enabled", candidate.loraAdrEnabled) ||
      !parseBoolArg("hop_enabled", candidate.loraHopEnabled)) {
    res.sendText(400, "invalid boolean configuration"); return;
  }
  if (req.hasArg("hop_profile")) {
    if (!parseUnsigned(req.arg("hop_profile"), Config::HOP_CHANNEL_MAX, value) ||
        value == 0) {
      res.sendText(400, "invalid hop profile"); return;
    }
    candidate.loraHopChannelProfile = static_cast<uint8_t>(value);
  }
  if (req.hasArg("range_test_mode")) {
    const String raw = req.arg("range_test_mode");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid range-test mode"); return;
    }
    candidate.loraRangeTestMode = raw == "1";
  }

  // ---- BLE reader ----
  if (!parseBoolArg("ble_enabled", candidate.sensorReaderEnabled)) {
    res.sendText(400, "invalid BLE reader setting"); return;
  }
  if (req.hasArg("ble_scan_interval_ms")) {
    if (!parseUnsigned(req.arg("ble_scan_interval_ms"),
                       Config::BLE_SCAN_INTERVAL_MS_MAX, value) ||
        value < Config::BLE_SCAN_INTERVAL_MS_MIN) {
      res.sendText(400, "invalid BLE scan interval"); return;
    }
    candidate.sensorScanIntervalMs = value;
  }
  if (req.hasArg("ble_scan_window_ms")) {
    if (!parseUnsigned(req.arg("ble_scan_window_ms"), 60000, value) ||
        value == 0) {
      res.sendText(400, "invalid BLE scan window"); return;
    }
    candidate.sensorScanWindowMs = static_cast<uint16_t>(value);
  }
  if (req.hasArg("ble_scan_duration_ms")) {
    if (!parseUnsigned(req.arg("ble_scan_duration_ms"), 60000, value) ||
        value < 100) {
      res.sendText(400, "invalid BLE scan duration"); return;
    }
    candidate.sensorScanDurationMs = value;
  }
  if (req.hasArg("ble_connect_timeout_ms")) {
    if (!parseUnsigned(req.arg("ble_connect_timeout_ms"), 30000, value) ||
        value < 500) {
      res.sendText(400, "invalid BLE connect timeout"); return;
    }
    candidate.sensorConnectTimeoutMs = value;
  }
  if (req.hasArg("ble_eviction_ms")) {
    if (!parseUnsigned(req.arg("ble_eviction_ms"), 7UL * 86400000UL, value) ||
        value < 10000) {
      res.sendText(400, "invalid BLE eviction timeout"); return;
    }
    candidate.sensorNodeEvictionMs = value;
  }
  if (req.hasArg("ble_max_nodes")) {
    if (!parseUnsigned(req.arg("ble_max_nodes"),
                       Config::SENSOR_MAX_NODES_VALUE, value) || value == 0) {
      res.sendText(400, "invalid BLE maximum nodes"); return;
    }
    candidate.sensorMaxNodes = static_cast<uint8_t>(value);
  }
  if (!parseBoolArg("ble_encryption", candidate.sensorRequireEncryption)) {
    res.sendText(400, "invalid BLE encryption setting"); return;
  }
  if (req.hasArg("ble_failure_threshold")) {
    if (!parseUnsigned(req.arg("ble_failure_threshold"), 20, value) || value == 0) {
      res.sendText(400, "invalid BLE pairing failure threshold"); return;
    }
    candidate.blePairingFailureThreshold = static_cast<uint8_t>(value);
  }
  if (req.hasArg("ble_block_ms")) {
    if (!parseUnsigned(req.arg("ble_block_ms"), 86400000UL, value) ||
        value < 1000) {
      res.sendText(400, "invalid BLE pairing block time"); return;
    }
    candidate.blePairingBlockMs = value;
  }
  if (!parseBoolArg("ble_keep_awake", candidate.sensorKeepAwake)) {
    res.sendText(400, "invalid BLE keep-awake setting"); return;
  }

  // ---- Web security policy ----
  if (req.hasArg("web_session_timeout_ms")) {
    if (!parseUnsigned(req.arg("web_session_timeout_ms"),
                       Config::WEB_SESSION_TIMEOUT_MS_MAX, value) ||
        value < Config::WEB_SESSION_TIMEOUT_MS_MIN) {
      res.sendText(400, "invalid web session timeout"); return;
    }
    candidate.webSessionTimeoutMs = value;
  }
  if (req.hasArg("web_auth_rate_limit_ms")) {
    if (!parseUnsigned(req.arg("web_auth_rate_limit_ms"),
                       Config::WEB_AUTH_RATE_LIMIT_MS_MAX, value) ||
        value < Config::WEB_AUTH_RATE_LIMIT_MS_MIN) {
      res.sendText(400, "invalid auth rate limit"); return;
    }
    candidate.webAuthRateLimitMs = value;
  }
  if (req.hasArg("csrf_policy")) {
    if (!parseUnsigned(req.arg("csrf_policy"), 1, value)) {
      res.sendText(400, "invalid CSRF policy"); return;
    }
    candidate.csrfPolicy = static_cast<uint8_t>(value);
  }
  if (req.hasArg("ble_pairing_policy")) {
    if (!parseUnsigned(req.arg("ble_pairing_policy"), 1, value)) {
      res.sendText(400, "invalid BLE pairing policy"); return;
    }
    candidate.blePairingPolicy = static_cast<uint8_t>(value);
  }
  if (req.hasArg("ecdh_rekey_policy")) {
    if (!parseUnsigned(req.arg("ecdh_rekey_policy"), 1, value)) {
      res.sendText(400, "invalid ECDH rekey policy"); return;
    }
    candidate.ecdhRekeyPolicy = static_cast<uint8_t>(value);
  }
  if (req.hasArg("replay_window_bits")) {
    if (!parseUnsigned(req.arg("replay_window_bits"),
                       Config::LORA_REPLAY_WINDOW_BITS, value) ||
        value < Config::LORA_REPLAY_WINDOW_BITS_MIN) {
      res.sendText(400, "replay window must be 8..32 bits"); return;
    }
    candidate.replayWindowBits = static_cast<uint8_t>(value);
  }

  // ---- Cross-field validation ----
  if (!candidate.validSemantics() || candidate.volume > 100 ||
      candidate.audioRecordSource > Config::AUDIO_SOURCE_USB ||
      candidate.wakePeriodSec < Config::WAKE_PERIOD_SEC_MIN ||
      candidate.wakePeriodSec > Config::WAKE_PERIOD_SEC_MAX ||
      candidate.deepSleepIdleMs < 60000UL ||
      candidate.deepSleepIdleMs > 86400000UL ||
      candidate.deepSleepWakeGraceMs < 100UL ||
      candidate.deepSleepWakeGraceMs > 60000UL ||
      candidate.criticalShutdownDelayMs < 100UL ||
      candidate.criticalShutdownDelayMs > 600000UL ||
      !isfinite(candidate.batteryLowThreshold) ||
      !isfinite(candidate.batteryCriticalThreshold) ||
      candidate.batteryCriticalThreshold < Config::BATTERY_CRITICAL_THRESHOLD_MIN ||
      candidate.batteryLowThreshold <= candidate.batteryCriticalThreshold ||
      candidate.batteryLowThreshold > Config::BATTERY_LOW_THRESHOLD_MAX ||
      candidate.classDBoostLevel > 7 ||
      candidate.staSsid.length() > Config::STA_SSID_MAX_LEN ||
      ((!candidate.staSsid.isEmpty()) &&
       (candidate.staPassword.length() < Config::STA_PASSWORD_MIN_LEN ||
        candidate.staPassword.length() > Config::STA_PASSWORD_MAX_LEN)) ||
      (candidate.classDEnabled && !Config::CLASS_D_ENABLED) ||
      !validHex32(candidate.loraKeyHex) ||
      !validPassword(candidate.apPassword) ||
      (!candidate.webPassword.isEmpty() && !validPassword(candidate.webPassword)) ||
      candidate.apSsid.isEmpty() || candidate.webUser.isEmpty() ||
      !candidate.webPasswordConfigured() ||
      (!candidate.webPassword.isEmpty() &&
       candidate.apPassword == candidate.webPassword)) {
    res.sendText(400, "invalid configuration");
    return;
  }

  RuntimeConfig previous{};
  if (!configSnapshot(previous)) {
    res.send503("configuration snapshot unavailable");
    return;
  }
  const uint8_t previousSource = audio.recordSource();

  const bool committed = configApplyTransaction(
      candidate, configGenerationSnapshot,
      [&]() {
        if (candidate.audioRecordSource != previousSource &&
            !audio.setRecordSource(candidate.audioRecordSource)) return false;
        if (!audio.setClassDConfig(candidate.classDEnabled,candidate.classDBoostLevel) ||
            !audio.setVox(candidate.voxEnabled,candidate.voxThreshold,candidate.voxHangMs) ||
            !audio.setAec(candidate.aecEnabled) ||
            !audio.setUsbMonitor(candidate.usbMonitor) ||
            !audio.setUsbPlaybackTransport(candidate.usbPlaybackTransport) ||
            !audio.setLoopback(candidate.audioLoopback)) return false;
        if (!mqtt.applyConfig(candidate)) return false;
        if (!lora.setAdrEnabled(candidate.loraAdrEnabled)) return false;
        if (radioChanged && !lora.applyConfig(candidate)) return false;
        audio.setVolume(candidate.volume);
        return true;
      },
      [&]() {
        bool ok = true;
        ok = audio.setClassDConfig(previous.classDEnabled, previous.classDBoostLevel) && ok;
        ok = audio.setVox(previous.voxEnabled, previous.voxThreshold, previous.voxHangMs) && ok;
        ok = audio.setAec(previous.aecEnabled) && ok;
        ok = audio.setUsbMonitor(previous.usbMonitor) && ok;
        ok = audio.setUsbPlaybackTransport(previous.usbPlaybackTransport) && ok;
        ok = audio.setLoopback(previous.audioLoopback) && ok;
        ok = audio.setRecordSource(previousSource) && ok;
        ok = lora.setAdrEnabled(previous.loraAdrEnabled) && ok;
        ok = mqtt.applyConfig(previous) && ok;
        if (radioChanged) ok = lora.applyConfig(previous) && ok;
        audio.setVolume(previous.volume);
        return ok;
      });

  if (!committed) {
    res.send503("configuration transaction failed; runtime and persistence rolled back");
    return;
  }
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.rangeTest = candidate.loraRangeTestMode;
      const uint8_t profileCount = min<uint8_t>(
          max<uint8_t>(1U, candidate.loraHopChannelProfile),
          Config::HOP_CHANNEL_MAX);
      gState.hopChannelCount = profileCount;
      for (uint8_t i = 0; i < Config::HOP_CHANNEL_MAX; ++i)
        gState.hopChannelList[i] = i < profileCount ? i : 0;
    }
  }
  lora.updateSourceId();
  auditConfigChange(previous, gConfig, "web");
  res.sendText(200,
               "Configuration saved; BLE pairing and WiFi credential changes apply after reboot");
}
