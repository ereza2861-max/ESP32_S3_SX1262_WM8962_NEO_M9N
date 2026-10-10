Iterasi C — Pesan 5 dari 7: WebUi.cpp Bagian 1

Peringatan Format

Ini pesan terpanjang. Saya bagi WebUi.cpp menjadi 3 bagian karena totalnya ~4000 baris:

· Pesan 5 (ini): Bagian 1 — header include, free helpers, begin() dengan 121 registrasi, task(), auth/session/CSRF helpers.
· Pesan 6: Bagian 2 — handler 1 sampai ~70 (status, LoRaWAN, files, upload, messages, track, scan, hop, sensor).
· Pesan 7: Bagian 3 — handler ~70 sampai 121 (config, factory reset, MQTT, audio, BLE passkey, diag) + main.cpp + platformio.ini.

Anda akan menyambung ketiga bagian menjadi satu WebUi.cpp. Saya akan tandai di akhir tiap bagian dengan komentar // === SAMBUNG KE BAGIAN N+1 === yang harus Anda hapus (atau ganti dengan newline) saat menyambung.

Yang Perlu Anda Tahu Sebelum Salin

Perubahan besar dari versi lama

1. server_.on() → server_.on() tapi tipenya HttpdServer, sehingga handler signature jadi [this](HttpdRequest& req, HttpdResponse& res){...} bukan [this]{...}.
2. server_.send(code, mime, body) → res.send(code, mime, body).
3. server_.sendHeader(k, v) → res.setHeader(k, v).
4. server_.arg(name) → req.arg(name).
5. server_.hasArg(name) → req.hasArg(name).
6. server_.header(name) → req.header(name).
7. server_.method() → req.method() — return httpd_method_t (HTTP_GET, HTTP_POST, dst), bukan HTTPMethod lama. Tapi nilai enumnya sama nama, jadi perbandingan req.method() == HTTP_POST tetap valid.
8. server_.client().remoteIP() → req.remoteIp().
9. server_.contentLength() → req.contentLength().
10. server_.requestAuthentication() → res.send401Basic(...).
11. server_.upload() → HttpdMultipart::parse.
12. server_.streamFile(f, mime) → res.streamFile(f, mime).
13. server_.setServerKeyAndCert(...) → server_.begin(...) di begin().
14. server_.begin() → dihapus, sudah dipanggil di begin() dengan server_.begin(cert, len, key, len, port).
15. server_.handleClient() → dihapus dari task().

Yang saya tahan untuk iterasi berikutnya (bukan sekarang)

Untuk menjaga Pesan 5 tetap terbaca, saya belum menyertakan:

· Free function encryptConfigBackup/decryptConfigBackup — dibutuhkan handleConfigBackup/handleConfigRestore di Pesan 6. Saya akan tulis di Pesan 6 sebelum handler terkait.
· Free function eraseStorageTree, isValidUploadedWav — dibutuhkan handleUpload dan handleFactoryReset. Saya tulis di Pesan 6.
· sensorNodeJson, sensorAddressJson — dibutuhkan handler sensor. Saya tulis di Pesan 6.

Semua itu tetap ada di file final, hanya saja ditulis di bagian yang sesuai.

---

Salin ke src/WebUi.cpp

Kosongkan dulu src/WebUi.cpp lama, lalu salin Bagian 1 ini:

```cpp
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
```

Catatan: Di atas saya tulis const INDEX_HTML[] PROGMEM (array char) sedangkan versi lama Anda static const char INDEX_HTML[] PROGMEM = R"HTML(...)HTML";. Saya pertahankan const char — supaya konsisten dengan pemakaian FPSTR(INDEX_HTML) di handleRoot. Kalau build error tentang FPSTR dan const char[], kita ganti.

Karena pesan ini sudah sangat panjang, saya putus di sini. Lanjut ke bagian begin(), task(), dan helper auth di Pesan 5b (masih bagian dari file yang sama), agar tidak ada yang terpotong.

Konfirmasi kalau Anda sudah salin blok di atas (sampai )HTML";) — saya kirim Pesan 5b: begin(), task(), auth/session/CSRF helpers, dan free function eraseStorageTree, isValidUploadedWav, sensorAddressJson, sensorNodeJson, encryptConfigBackup, decryptConfigBackup, parseBackupLine.