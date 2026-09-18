#include "WebUi.h"
#include "generated/WebTlsProvisioning.h"
#include "Config.h"
#include "AppState.h"
#include "StorageManager.h"
#include "LoRaManager.h"
#include "LoRaWANManager.h"
#include "AudioManager.h"
#include "PersistentConfig.h"
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

static String configBackupPlaintext() {
  const RuntimeConfig& c = gConfig;
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
  p += "webuser=" + c.webUser + "\n";
  p += "websalt=" + c.webPasswordSaltHex + "\n";
  p += "webph=" + c.webPasswordHashHex + "\n";
  return p;
}

static bool backupKey(uint8_t key[32]) {
  if (!key || gConfig.webPasswordHashHex.length() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const char a = gConfig.webPasswordHashHex[i * 2];
    const char b = gConfig.webPasswordHashHex[i * 2 + 1];
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

extern StorageManager storage;
extern LoRaManager lora;
extern LoRaWANManager lorawan;
extern AudioManager audio;
extern BleSensorReader bleSensorReader;
static SensorReader::SensorNodeSnapshot gSensorSnapshots[SensorRegistry::MAX_SUPPORTED_NODES]{};

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>FieldRadio</title><style>
body{font-family:sans-serif;max-width:1100px;margin:auto;padding:16px;background:#111;color:#eee}
body.light{background:#f5f5f5;color:#111} body.light .card{border-color:#bbb} body.light pre{background:#e8e8e8}
.card{border:1px solid #444;border-radius:8px;padding:12px;margin:8px 0}
@media(max-width:600px){body{padding:8px}.card{padding:8px}button,input,select{width:100%;box-sizing:border-box;margin:3px 0}table{font-size:.8rem;display:block;overflow-x:auto}}
button,input{font-size:1rem;margin:4px;padding:10px}pre{background:#222;padding:10px;overflow:auto}
.battery-low{outline:3px solid orange}.battery-critical{outline:4px solid red}.sensor-badge{padding:2px 5px;border-radius:4px;font-size:.75rem;border:1px solid #777}.sensor-badge.q0{background:#164d25}.sensor-badge:not(.q0){background:#6a4b00}
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
<label>Loopback <input id=loop type=checkbox onchange="setLoopback()"></label><label>VOX threshold <input id=voxThreshold type=number step="0.01" min="0.01" max="1" value="0.08"></label><label>hang ms <input id=voxHang type=number min="50" max="5000" value="700"></label><label>AEC <input id=aec type=checkbox onchange="setAec()"></label><label>VOX <input id=vox type=checkbox onchange="setVox()"></label><label>Record quality <select id=recordQuality onchange="setRecordQuality()"><option value="low">8k mono</option><option value="medium">16k mono</option><option value="high" selected>44.1k stereo</option></select></label>
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
<input id=aps value="" placeholder="AP password"><input id=wp value="" placeholder="Web password">
<button onclick="saveCfg()">Save config</button><button onclick="reboot()">Reboot</button><button onclick="factoryReset()">Factory reset</button></div>
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
<div class=card><button onclick="toggleTheme()">Dark/light</button><button onclick="showTrack()">Track</button><span id=toast></span></div>
<div class=card id=trackPanel style="display:none"><h3>Track</h3>
<label>From epoch <input id=trackFrom type=number value="0"></label>
<label>To epoch <input id=trackTo type=number value=""></label>
<label>Limit <input id=trackLimit type=number min="1" max="5000" value="1000"></label>
<button onclick="loadTrack()">Load track</button><div id=trackMap style="height:420px"></div></div>
<div class=card><h3>Status</h3><pre id=s></pre></div>
<div class=card><h3>Files</h3><input id=fileDir value="/REC/"><button onclick="refreshFiles()">Open folder</button><pre id=f></pre>
<input id=upfile type=file accept=".wav,.WAV"><button onclick="uploadFile()">UPLOAD WAV</button>
<input id=renameFrom placeholder="/REC/old.WAV"><input id=renameTo placeholder="/REC/new.WAV"><button onclick="renameFile()">RENAME</button>
<a id=trackDownload href="/api/track/download">Download GPS track</a>
</div>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
const CSRF_TOKEN='__CSRF_TOKEN__';
let trackMap=null,trackLayer=null;
function sensorBadge(q){let a=[];if(q&1)a.push('STALE');if(q&2)a.push('RANGE');if(q&4)a.push('GW-TS');if(q&8)a.push('BAD-TS');if(q&16)a.push('LINK');return `<span class="sensor-badge q${q}">${a.length?a.join(' '):'VALID'}</span>`}
function sensorEscape(v){return String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function sensorAction(id,action){if(!confirm(action==='forget'?'Forget this sensor node?':'Reconnect this sensor node?'))return;try{const r=await fetch(`/api/sensors/${action}?id=${encodeURIComponent(id)}`,{method:'POST',headers:{'X-CSRF-Token':CSRF_TOKEN}});toast(await r.text());await refreshSensorNodes()}catch(e){toast('Sensor action failed')}}
async function refreshSensorDetail(id){try{const n=await (await fetch('/api/sensors/nodes?id='+encodeURIComponent(id))).json();if(!n.ok){sensorDetail.textContent=n.error||'Node unavailable';return}let h=`<h4>Node ${n.id} — ${sensorEscape(n.name||'(unnamed)')}</h4><table><thead><tr><th>ID</th><th>Name</th><th>Unit</th><th>Value</th><th>Timestamp</th><th>Quality</th></tr></thead><tbody>`;for(const x of n.sensors||[]){h+=`<tr><td>${x.id}</td><td>${sensorEscape(x.name)}</td><td>${sensorEscape(x.unit)}</td><td>${x.valueValid?x.value:'—'}</td><td>${x.timestamp||'—'}</td><td>${sensorBadge(x.quality)}</td></tr>`}sensorDetail.dataset.nodeId=String(n.id);sensorDetail.innerHTML=h+'</tbody></table>'}catch(e){toast('Sensor detail failed')}}
async function refreshSensorNodes(){try{const a=await (await fetch('/api/sensors/nodes')).json();sensorNodesBody.innerHTML=(a.nodes||[]).map(n=>`<tr><td><button onclick="refreshSensorDetail(${n.id})">${n.id}</button></td><td>${sensorEscape(n.address)}</td><td>${n.rssi}</td><td>${n.sensorCount}</td><td>${n.lastSeenMs} ms</td><td>${n.connected?'CONNECTED':'OFFLINE'}</td><td><button onclick="sensorAction(${n.id},'refresh')">Refresh</button><button onclick="sensorAction(${n.id},'refresh')">Reconnect</button><button onclick="sensorAction(${n.id},'forget')">Forget</button></td></tr>`).join('')}catch(e){toast('Sensor inventory failed')}}
async function refreshSensorLive(){try{const a=await (await fetch('/api/sensors/live')).json();if(a.nodes) for(const n of a.nodes){const open=document.getElementById('sensorDetail');if(open.dataset.nodeId==n.id) await refreshSensorDetail(n.id)}}catch(e){}}
refreshSensorNodes();setInterval(refreshSensorNodes,5000);setInterval(refreshSensorLive,2000);

async function j(u,o={}){o.headers=Object.assign({},o.headers||{},o.method&&o.method.toUpperCase()!=='GET'?{'X-CSRF-Token':CSRF_TOKEN}:{});let r=await fetch(u,o);return await r.text()}
function toast(t){document.getElementById('toast').textContent=t;setTimeout(()=>document.getElementById('toast').textContent='',2500)}
async function setLang(){const c=document.getElementById('lang').value;localStorage.setItem('fieldradio-lang',c);try{await fetch('/api/lang?set='+c)}catch(e){};document.getElementById('title').textContent=c==='id'?'FieldRadio':'FieldRadio'}
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
function showTrack(){
  const p=document.getElementById('trackPanel'); p.style.display=p.style.display==='none'?'block':'none';
  if(p.style.display==='block'){
    if(!trackMap){trackMap=L.map('trackMap');L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'© OpenStreetMap'}).addTo(trackMap);}
    setTimeout(()=>trackMap.invalidateSize(),50);
  }
}
async function loadTrack(){
  try{
    const q=new URLSearchParams({from:trackFrom.value||'0',to:trackTo.value||'18446744073709551615',limit:String(Math.min(5000,Math.max(1,Number(trackLimit.value)||1000)))});
    const a=await (await fetch('/api/track/points?'+q)).json();
    if(!trackMap){showTrack();}
    if(trackLayer)trackLayer.clearLayers(); else trackLayer=L.layerGroup().addTo(trackMap);
    if(!a.length){toast('No track points');return;}
    const latlng=a.map(x=>[x.lat,x.lon]);
    L.polyline(latlng).addTo(trackLayer);
    L.marker(latlng[0]).addTo(trackLayer).bindPopup('Start');
    L.marker(latlng[latlng.length-1]).addTo(trackLayer).bindPopup('End');
    trackMap.fitBounds(L.latLngBounds(latlng),{padding:[20,20]});
  }catch(e){toast('Track load failed')}
}
async function refreshSosHistory(){try{const a=await (await fetch('/api/sos-history')).json();sosBadge.textContent=a.map(x=>`event=${x.event} seq=${x.seq} peer=${x.peer}`).join(' | ')}catch(e){}}
async function refreshFiles(){try{f.textContent=await j('/api/files?dir='+encodeURIComponent(fileDir.value))}catch(e){toast('File list failed')}}
async function refresh(){
  const raw=await j('/api/status');s.textContent=raw;await refreshFiles();
  try{const x=JSON.parse(raw);document.getElementById('unreadBadge').textContent=(x.messageUnread||0)+' unread';document.getElementById('sosBadge').textContent=x.sosEscalated?'SOS ESCALATED':(x.sos?'SOS ACTIVE':'');document.body.classList.toggle('battery-low',!!x.battery?.low);document.body.classList.toggle('battery-critical',!!x.battery?.critical);
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
    audio_source:audsrc.value});
  if(key.value)q.set('lora_key',key.value);
  if(aps.value)q.set('ap_password',aps.value);
  if(wp.value)q.set('web_password',wp.value);
  alert(await j('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q}));refresh()
}
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
setInterval(refresh,1000);syncSource();refresh();refreshMessages();refreshRadioHistory();refreshSosHistory()
setInterval(refreshLw,2000);refreshLw();setInterval(refreshScan,3000);setInterval(refreshHop,3000);setInterval(refreshMessages,2000);setInterval(refreshRadioHistory,3000);refreshScan();refreshHop()
</script></body></html>)HTML";

bool WebUi::sameOrigin() {
  const String origin = server_.header("Origin");
  if (origin.isEmpty() || !origin.startsWith("https://")) return false;

  // Accept the actual HTTPS Host as well as the AP IP. The UI certificate has
  // both SANs, so rejecting the DNS host here would make all state-changing
  // controls fail when the user opens https://fieldradio.local/.
  const String host = server_.header("Host");
  const String apOrigin = String("https://") + WiFi.softAPIP().toString();
  if (origin == apOrigin) return true;

  if (!host.isEmpty()) {
    String expected = String("https://") + host;
    // HTTPS uses port 443 by default; keep an explicit :443 equivalent.
    if (expected.endsWith(":443"))
      expected.remove(expected.length() - 4);
    return origin == expected;
  }
  return false;
}

bool WebUi::rateLimit(uint32_t& last, uint32_t interval) {
  struct RateSlot {
    uintptr_t endpoint = 0;
    String ip;
    uint32_t last = 0;
  };
  static RateSlot slots[32];
  const uint32_t now = millis();
  const String ip = server_.client().remoteIP().toString();
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
    server_.send(429, "text/plain", "rate limited");
    return false;
  }
  slot->last = now;
  last = now;
  return true;
}

bool WebUi::sessionValid() {
  const String cookie = server_.header("Cookie");
  const String prefix = "FR-SESSION=";
  const int start = cookie.indexOf(prefix);
  if (start < 0 || !sessionSecretReady_) return false;
  const int end = cookie.indexOf(';', start);
  const String token = cookie.substring(start + prefix.length(),
                                         end < 0 ? cookie.length() : end);
  if (token.length() != 64) return false;

  uint8_t raw[32] = {};
  for (size_t i = 0; i < 32; ++i) {
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

  // The token is HMAC-SHA256(secret, client-IP || issue-time).
  // The signing secret exists only in RAM, so a reboot invalidates all sessions.
  uint8_t expected[32] = {};
  const IPAddress ip = server_.client().remoteIP();
  uint8_t msg[8] = {};
  const uint32_t ipValue = static_cast<uint32_t>(ip[0]) |
                           (static_cast<uint32_t>(ip[1]) << 8) |
                           (static_cast<uint32_t>(ip[2]) << 16) |
                           (static_cast<uint32_t>(ip[3]) << 24);
  memcpy(msg, &ipValue, sizeof(ipValue));
  memcpy(msg + 4, &sessionIssuedMs_, sizeof(sessionIssuedMs_));
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, sessionSecret_, sizeof(sessionSecret_),
                             msg, sizeof(msg), expected, sizeof(expected)) != 0)
    return false;
  uint8_t tokenDiff = 0;
  for (size_t i = 0; i < sizeof(raw); ++i)
    tokenDiff |= static_cast<uint8_t>(raw[i] ^ expected[i]);
  if (tokenDiff != 0) return false;
  return static_cast<int32_t>(millis() - authBlockedUntilMs_) >= 0 &&
         static_cast<uint32_t>(millis() - sessionIssuedMs_) <
             Config::WEB_SESSION_TIMEOUT_MS;
}

bool WebUi::csrfValid() {
  if (server_.method() != HTTP_POST && server_.method() != HTTP_DELETE) return false;
  if (csrfTokenHex_.isEmpty() || csrfTokenHex_.length() != 32) {
    ++csrfFailures_;
    auditAuth(false);
    return false;
  }
  const String supplied = server_.header("X-CSRF-Token");
  if (supplied.length() != 32 || supplied.length() != csrfTokenHex_.length()) {
    ++csrfFailures_;
    (void)auditAuth(false);
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < supplied.length(); ++i)
    diff |= static_cast<uint8_t>(supplied[i] ^ csrfTokenHex_[i]);
  if (diff != 0) {
    ++csrfFailures_;
    auditAuth(false);
    return false;
  }
  return true;
}

bool WebUi::issueSession() {
  const IPAddress ip = server_.client().remoteIP();
  uint8_t msg[8] = {};
  const uint32_t ipValue = static_cast<uint32_t>(ip[0]) |
                           (static_cast<uint32_t>(ip[1]) << 8) |
                           (static_cast<uint32_t>(ip[2]) << 16) |
                           (static_cast<uint32_t>(ip[3]) << 24);
  sessionIssuedMs_ = millis();
  memcpy(msg, &ipValue, sizeof(ipValue));
  memcpy(msg + 4, &sessionIssuedMs_, sizeof(sessionIssuedMs_));
  uint8_t token[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, sessionSecret_, sizeof(sessionSecret_),
                             msg, sizeof(msg), token, sizeof(token)) != 0)
    return false;
  String hex;
  hex.reserve(64);
  const char* digits = "0123456789abcdef";
  for (uint8_t b : token) {
    hex += digits[b >> 4];
    hex += digits[b & 0x0F];
  }
  csrfTokenHex_.reserve(sizeof(csrfToken_) * 2);
  csrfTokenHex_ = String();
  for (uint8_t& b : csrfToken_) {
    b = static_cast<uint8_t>(esp_random() & 0xFFU);
    csrfTokenHex_ += digits[b >> 4];
    csrfTokenHex_ += digits[b & 0x0F];
  }
  server_.sendHeader("Set-Cookie",
      "FR-SESSION=" + hex + "; Max-Age=" +
      String(Config::WEB_SESSION_TIMEOUT_MS / 1000) +
      "; HttpOnly; Secure; SameSite=Strict");

  return true;
}

void WebUi::auditAuth(bool success) {
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
    f.printf("%lu,%s,%s\\n",
             static_cast<unsigned long>(now),
             server_.client().remoteIP().toString().c_str(),
             success ? "AUTH_OK" : "AUTH_FAIL");
    f.close();
  }
}

static bool basicAuthMatches(WebServer& server, const String& user,
                              const RuntimeConfig& config) {
  const String header = server.header("Authorization");
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

bool WebUi::auth() {
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - authBlockedUntilMs_) < 0) {
    server_.send(429, "text/plain", "too many authentication failures");
    return false;
  }

  if (!sessionValid()) {
    if (!basicAuthMatches(server_, gConfig.webUser, gConfig)) {
      if (now - authFailureWindowStartMs_ >= 60000) {
        authFailureWindowStartMs_ = now;
        authFailures_ = 0;
      }
      ++authFailures_;
      ++authFailureWindowCount_;
      auditAuth(false);
      if (authFailures_ >= 5) {
        authBlockedUntilMs_ = now + 30000;
        authFailures_ = 0;
      }
      server_.requestAuthentication();
      return false;
    }
    authFailures_ = 0;
    authFailureWindowCount_ = 0;
    authFailureWindowStartMs_ = now;
    if (!issueSession()) {
      auditAuth(false);
      server_.send(503, "text/plain", "session initialization failed");
      return false;
    }
    auditAuth(true);
  }
  if (server_.method() == HTTP_POST || server_.method() == HTTP_DELETE) {
    if (!sameOrigin()) {
      server_.send(403, "text/plain", "forbidden origin");
      return false;
    }
    if (!csrfValid()) {
      server_.send(403, "text/plain", "invalid CSRF token");
      return false;
    }
  }
  return true;
}

void WebUi::begin() {
  for (size_t i = 0; i < sizeof(sessionSecret_); i += 4) {
    const uint32_t r = esp_random();
    memcpy(sessionSecret_ + i, &r, min<size_t>(4, sizeof(sessionSecret_) - i));
  }
  sessionSecretReady_ = true;
  // ESPWebServerSecure exposes request headers directly through header();
  // collectHeaders() is intentionally not used because the HTTPS compatibility
  // layer does not implement the WebServer header collection cache.

  server_.on("/", HTTP_GET, [this]{ if (auth()) handleRoot(); });
  server_.on("/api/status", HTTP_GET, [this]{ if (auth()) handleStatus(); });
  server_.on("/api/v1/status", HTTP_GET, [this]{ if (auth()) handleStatus(); });
  server_.on("/api/lorawan/status", HTTP_GET, [this]{ if (auth()) handleLoRaWANStatus(); });
  server_.on("/api/lorawan/connect", HTTP_POST, [this]{ if (auth()) handleLoRaWANConnect(); });
  server_.on("/api/lorawan/disconnect", HTTP_POST, [this]{ if (auth()) handleLoRaWANDisconnect(); });
  server_.on("/api/lorawan/config", HTTP_POST, [this]{ if (auth()) handleLoRaWANConfig(); });
  server_.on("/api/lorawan/uplink", HTTP_POST, [this]{ if (auth()) handleLoRaWANUplink(); });
  server_.on("/api/version", HTTP_GET, [this]{ if (auth()) handleApiVersion(); });
  server_.on("/api/v1/version", HTTP_GET, [this]{ if (auth()) handleApiVersion(); });
  server_.on("/api/v1/csrf", HTTP_GET, [this]{ if (auth()) server_.send(200, "application/json", "{\"token\":\"" + csrfTokenHex_ + "\"}"); });
  server_.on("/api/files", HTTP_GET, [this]{ if (auth()) handleFiles(); });
  server_.on("/api/download", HTTP_GET, [this]{ if (auth()) handleDownload(); });
  server_.on("/api/upload", HTTP_POST, [this]{ if (auth()) {
    if (uploadFailed_) { server_.send(400, "text/plain", "upload failed"); return; }
    server_.send(200, "text/plain", "OK");
  }}, [this]{ if (auth()) handleUpload(); });
  server_.on("/api/rename", HTTP_POST, [this]{ if (auth()) handleRename(); });
  server_.on("/api/messages", HTTP_GET, [this]{ if (auth()) handleMessages(); });
  server_.on("/api/messages/clear", HTTP_POST, [this]{ if (auth()) handleMessageClear(); });
  server_.on("/api/messages/read", HTTP_POST, [this]{ if (auth()) handleMessageRead(); });
  server_.on("/api/messages/reply", HTTP_POST, [this]{ if (auth()) handleMessageReply(); });
  server_.on("/api/messages/export", HTTP_GET, [this]{ if (auth()) handleMessageExport(); });
  server_.on("/api/messages/persist", HTTP_GET, [this]{ if (auth()) handleMessagePersist(); });
  server_.on("/api/message/schedule", HTTP_POST, [this]{ if (auth()) handleMessageSchedule(); });
  server_.on("/api/message/schedule", HTTP_GET, [this]{ if (auth()) handleMessageScheduleList(); });
  server_.on("/api/message/schedule", HTTP_DELETE, [this]{ if (auth()) handleMessageScheduleDelete(); });
  server_.on("/api/record/schedule", HTTP_POST, [this]{ if (auth()) handleRecordSchedule(); });
  server_.on("/api/record/schedule", HTTP_GET, [this]{ if (auth()) handleRecordScheduleGet(); });
  server_.on("/api/sos/format", HTTP_POST, [this]{ if (auth()) handleSosFormat(); });
  server_.on("/api/selftest", HTTP_POST, [this]{ if (auth()) handleSelfTest(); });
  server_.on("/api/selftest/result", HTTP_GET, [this]{ if (auth()) handleSelfTestResult(); });
  server_.on("/api/lang", HTTP_GET, [this]{ if (auth()) handleLang(); });
  server_.on("/api/neighbors", HTTP_GET, [this]{ if (auth()) handleNeighbors(); });
  server_.on("/api/sensors/nodes", HTTP_GET, [this]{ if (auth()) {
    if (server_.hasArg("id")) handleSensorNodeDetail(); else handleSensorNodes();
  }});
  server_.on("/api/sensors/live", HTTP_GET, [this]{ if (auth()) handleSensorLive(); });
  server_.on("/api/sensors/forget", HTTP_POST, [this]{ if (auth()) handleSensorForget(); });
  server_.on("/api/sensors/refresh", HTTP_POST, [this]{ if (auth()) handleSensorRefresh(); });
  server_.on("/api/mqtt/provision", HTTP_POST, [this]{ if (auth()) handleMqttProvision(); });
  server_.on("/api/mqtt/status", HTTP_GET, [this]{ if (auth()) handleMqttStatus(); });
  server_.on("/api/routes", HTTP_GET, [this]{ if (auth()) handleRoutes(); });
  server_.on("/api/capture/start", HTTP_POST, [this]{ if (auth()) handleCaptureStart(); });
  server_.on("/api/capture/stop", HTTP_POST, [this]{ if (auth()) handleCaptureStop(); });
  server_.on("/api/capture/dump", HTTP_GET, [this]{ if (auth()) handleCaptureDump(); });
  server_.on("/api/adr", HTTP_POST, [this]{ if (auth()) handleAdr(); });
  server_.on("/api/hop/sync", HTTP_POST, [this]{ if (auth()) handleHopSync(); });
  server_.on("/api/dedup/stats", HTTP_GET, [this]{ if (auth()) handleDedupStats(); });
  server_.on("/api/forward/stats", HTTP_GET, [this]{ if (auth()) handleForwardStats(); });
  server_.on("/api/auth/stats", HTTP_GET, [this]{ if (auth()) handleAuthStats(); });
  server_.on("/api/theme", HTTP_POST, [this]{ if (auth()) handleTheme(); });
  server_.on("/api/nvs", HTTP_GET, [this]{ if (auth()) handleNvs(); });
  server_.on("/api/config/migrate", HTTP_POST, [this]{ if (auth()) handleConfigMigrate(); });
  server_.on("/api/battery/history", HTTP_GET, [this]{ if (auth()) handleBatteryHistory(); });
  server_.on("/api/radio/history", HTTP_GET, [this]{ if (auth()) handleRadioHistory(); });
  server_.on("/api/radio/stats", HTTP_GET, [this]{ if (auth()) handleRadioStats(); });
  server_.on("/api/rf/detector", HTTP_GET, [this]{ if (auth()) handleRfDetector(); });
  server_.on("/api/radio/tune", HTTP_POST, [this]{ if (auth()) handleRadioTune(); });
  server_.on("/api/range-test", HTTP_POST, [this]{ if (auth()) handleRangeTest(); });
  server_.on("/api/storage/info", HTTP_GET, [this]{ if (auth()) handleStorageInfo(); });
  server_.on("/api/storage/checksum", HTTP_GET, [this]{ if (auth()) handleChecksum(); });
  server_.on("/api/storage/checksum-sha256", HTTP_GET, [this]{ if (auth()) handleChecksumSha256(); });
  server_.on("/api/log/export", HTTP_GET, [this]{ if (auth()) handleLogExport(); });
  server_.on("/api/sos-history", HTTP_GET, [this]{ if (auth()) handleSosHistory(); });
  server_.on("/api/lora-log", HTTP_GET, [this]{ if (auth()) handleLoraLog(); });
  server_.on("/api/health-log", HTTP_GET, [this]{ if (auth()) handleHealthLog(); });
  server_.on("/api/battery-calibrate", HTTP_POST, [this]{ if (auth()) handleBatteryCalibrate(); });
  server_.on("/api/message", HTTP_POST, [this]{ if (auth()) handleMessage(); });
  server_.on("/api/sos", HTTP_POST, [this]{ if (auth()) handleSos(); });
  server_.on("/api/sos-status", HTTP_GET, [this]{ if (auth()) handleSosStatus(); });
  server_.on("/api/scan/status", HTTP_GET, [this]{ if (auth()) handleScanStatus(); });
  server_.on("/api/scan/start", HTTP_POST, [this]{ if (auth()) handleScanStart(); });
  server_.on("/api/scan/stop", HTTP_POST, [this]{ if (auth()) handleScanStop(); });
  server_.on("/api/scan/results", HTTP_GET, [this]{ if (auth()) handleScanResults(); });
  server_.on("/api/hop/suggest", HTTP_POST, [this]{ if (auth()) handleHopSuggest(); });
  server_.on("/api/hop/status", HTTP_GET, [this]{ if (auth()) handleHopStatus(); });
  server_.on("/api/hop/enable", HTTP_POST, [this]{ if (auth()) handleHopEnable(); });
  server_.on("/api/hop/set-channels", HTTP_POST, [this]{ if (auth()) handleHopSetChannels(); });
  server_.on("/api/ptt", HTTP_POST, [this]{ if (auth()) handlePtt(); });
  server_.on("/api/record", HTTP_POST, [this]{ if (auth()) handleRecord(); });
  server_.on("/api/record/quality", HTTP_POST, [this]{ if (auth()) handleRecordQuality(); });
  server_.on("/api/play", HTTP_POST, [this]{ if (auth()) handlePlay(); });
  server_.on("/api/stop", HTTP_POST, [this]{ if (auth()) handleStop(); });
  server_.on("/api/pause", HTTP_POST, [this]{ if (auth()) handlePause(); });
  server_.on("/api/seek", HTTP_POST, [this]{ if (auth()) handleSeek(); });
  server_.on("/api/queue", HTTP_POST, [this]{ if (auth()) handleQueue(); });
  server_.on("/api/queue-clear", HTTP_POST, [this]{ if (auth()) handleQueueClear(); });
  server_.on("/api/record-pause", HTTP_POST, [this]{ if (auth()) handleRecordPause(); });
  server_.on("/api/record-split", HTTP_POST, [this]{ if (auth()) handleRecordSplit(); });
  server_.on("/api/vox", HTTP_POST, [this]{ if (auth()) handleVox(); });
  server_.on("/api/vad", HTTP_POST, [this]{ if (auth()) handleVad(); });
  server_.on("/api/usb-transport", HTTP_POST, [this]{ if (auth()) handleUsbTransport(); });
  server_.on("/api/volume", HTTP_POST, [this]{ if (auth()) handleVolume(); });
  server_.on("/api/delete", HTTP_POST, [this]{ if (auth()) handleDelete(); });
  server_.on("/api/track", HTTP_GET, [this]{ if (auth()) handleTrack(); });
  server_.on("/api/track/points", HTTP_GET, [this]{ if (auth()) handleTrackPoints(); });
  server_.on("/api/track/simplified", HTTP_GET, [this]{ if (auth()) handleTrackSimplified(); });
  server_.on("/api/track/download", HTTP_GET, [this]{ if (auth()) handleTrackDownload(); });
  server_.on("/api/reboot", HTTP_POST, [this]{ if (auth()) handleReboot(); });
  server_.on("/api/config", HTTP_POST, [this]{ if (auth()) handleConfig(); });
  server_.on("/api/config/export", HTTP_GET, [this]{ if (auth()) handleConfigExport(); });
  server_.on("/api/config/backup", HTTP_GET, [this]{ if (auth()) handleConfigBackup(); });
  server_.on("/api/config/restore", HTTP_POST, [this]{ if (auth()) handleConfigRestore(); });
  server_.on("/api/factory-reset", HTTP_POST, [this]{ if (auth()) handleFactoryReset(); });
  server_.on("/api/audio-source", HTTP_POST, [this]{ if (auth()) handleAudioSource(); });
  server_.on("/api/audio-monitor", HTTP_POST, [this]{
    if (!auth()) return;
    const String raw = server_.arg("on");
    if (raw != "0" && raw != "1") { server_.send(400, "text/plain", "invalid monitor"); return; }
    const bool ok = audio.setUsbMonitor(raw == "1");
    server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "FAIL");
  });
  server_.on("/api/audio-loopback", HTTP_POST, [this]{
    if (!auth()) return;
    const String raw = server_.arg("on");
    if (raw != "0" && raw != "1") { server_.send(400, "text/plain", "invalid loopback"); return; }
    const bool ok = audio.setLoopback(raw == "1");
    server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "FAIL");
  });
  server_.on("/api/audio-aec", HTTP_POST, [this]{
    if (!auth()) return;
    const String raw = server_.arg("on");
    if (raw != "0" && raw != "1") { server_.send(400, "text/plain", "invalid aec"); return; }
    const bool ok = audio.setAec(raw == "1");
    server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "FAIL");
  });
  server_.on("/api/audio-tone", HTTP_POST, [this]{
    if (!auth()) return;
    const String rf = server_.arg("freq");
    const String rm = server_.arg("ms");
    if (rf.isEmpty() || rm.isEmpty() || rf.length() > 5 || rm.length() > 4) {
      server_.send(400, "text/plain", "invalid tone"); return;
    }
    const int f = rf.toInt();
    const int ms = rm.toInt();
    const bool ok = f >= 1 && f <= 10000 && ms >= 1 && ms <= static_cast<int>(Config::AUDIO_TONE_MAX_MS) &&
                   audio.playTone(static_cast<uint16_t>(f), static_cast<uint16_t>(ms));
    server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "FAIL");
  });
#if WEB_TLS_CERT_CONFIGURED
  server_.setServerKeyAndCert(WEB_TLS_KEY_DER, WEB_TLS_KEY_DER_LEN,
                              WEB_TLS_CERT_DER, WEB_TLS_CERT_DER_LEN);
  server_.begin();
#else
  // Never fall back to plaintext HTTP when certificate provisioning is absent.
  // The WebUI remains disabled until a device-specific certificate/key pair is
  // provisioned through the ignored secrets/ directory.
  Serial.println("WebUI HTTPS disabled: TLS certificate provisioning missing");
#endif
}

void WebUi::task() {
  server_.handleClient();
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

void WebUi::handleRoot() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("X-Content-Type-Options", "nosniff");
  server_.sendHeader("X-Frame-Options", "DENY");
  server_.sendHeader("Referrer-Policy", "no-referrer");
  server_.sendHeader("Content-Security-Policy",
                     "default-src 'self'; script-src 'self' 'unsafe-inline' https://unpkg.com; "
                     "style-src 'self' 'unsafe-inline' https://unpkg.com; object-src 'none'; "
                     "img-src 'self' data: https://*.tile.openstreetmap.org; "
                     "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'");
  String page = FPSTR(INDEX_HTML);
  page.replace("__CSRF_TOKEN__", csrfTokenHex_);
  Preferences themePrefs;
  String persistedTheme = "dark";
  if (themePrefs.begin("fieldradio", true)) { persistedTheme = themePrefs.getString("theme", "dark"); themePrefs.end(); }
  page.replace("__THEME_CLASS__", persistedTheme == "light" ? "light" : "");
  server_.send(200, "text/html", page);
}

void WebUi::handleApiVersion() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json",
               "{\"api\":1,\"protocol\":" +
               String(Config::LORA_PROTOCOL_VERSION) +
               ",\"secureBootV2\":" +
               String(CONFIG_SECURE_BOOT_V2_ENABLED ? "true" : "false") +
               ",\"flashEncryption\":" +
               String(CONFIG_SECURE_FLASH_ENC_ENABLED ? "true" : "false") +
               "}");
}

void WebUi::handleStatus() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }

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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleLoRaWANStatus() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  String j = "{\"enabled\":" + String(gConfig.lorawanEnabled ? "true" : "false") +
             ",\"mode\":" + String(gConfig.lorawanMode) +
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleLoRaWANConnect() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const bool ok = gConfig.lorawanMode == 0 ? lorawan.connectOTAA() : lorawan.connectABP();
  server_.send(ok ? 202 : 400, "text/plain", ok ? "LoRaWAN connect requested" : "LoRaWAN connect rejected");
}

void WebUi::handleLoRaWANDisconnect() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const bool ok = lorawan.disconnect();
  server_.send(ok ? 202 : 400, "text/plain", ok ? "LoRaWAN disconnect requested" : "LoRaWAN disconnect rejected");
}

void WebUi::handleLoRaWANConfig() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  RuntimeConfig candidate = gConfig;

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
      if (value > (maxValue - digit) / 10U) return false;
      value = value * 10U + digit;
    }
    out = value;
    return true;
  };

  if (server_.hasArg("enabled")) {
    const String v = server_.arg("enabled");
    if (v != "0" && v != "1") { server_.send(400, "text/plain", "invalid enabled"); return; }
    candidate.lorawanEnabled = v == "1";
  }
  if (server_.hasArg("mode")) {
    uint32_t v = 0;
    if (!parseU32(server_.arg("mode"), 1, v)) { server_.send(400, "text/plain", "invalid mode"); return; }
    candidate.lorawanMode = static_cast<uint8_t>(v);
  }
  if (server_.hasArg("region")) {
    uint32_t v = 0;
    if (!parseU32(server_.arg("region"), 3, v)) { server_.send(400, "text/plain", "invalid region"); return; }
    candidate.lorawanRegion = static_cast<uint8_t>(v);
  }
  if (server_.hasArg("deveui")) candidate.lorawanDevEui = server_.arg("deveui");
  if (server_.hasArg("joineui")) candidate.lorawanJoinEui = server_.arg("joineui");
  if (server_.hasArg("appkey")) candidate.lorawanAppKey = server_.arg("appkey");
  if (server_.hasArg("nwkskey")) candidate.lorawanNwkSKey = server_.arg("nwkskey");
  if (server_.hasArg("appskey")) candidate.lorawanAppSKey = server_.arg("appskey");

  if (server_.hasArg("devaddr")) {
    const String raw = server_.arg("devaddr");
    if (!hexField(raw, 8)) { server_.send(400, "text/plain", "invalid DevAddr"); return; }
    for (size_t i = 0; i < 4; ++i) {
      auto n = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = n(raw[i * 2]), lo = n(raw[i * 2 + 1]);
      if (hi < 0 || lo < 0) { server_.send(400, "text/plain", "invalid DevAddr"); return; }
      candidate.lorawanDevAddr[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
  }
  if (server_.hasArg("fport")) {
    uint32_t v = 0;
    if (!parseU32(server_.arg("fport"), 223, v) || v == 0) {
      server_.send(400, "text/plain", "invalid FPort"); return;
    }
    candidate.lorawanFPort = static_cast<uint8_t>(v);
  }
  if (server_.hasArg("period")) {
    uint32_t v = 0;
    if (!parseU32(server_.arg("period"), 86400, v) || v == 0) {
      server_.send(400, "text/plain", "invalid period"); return;
    }
    candidate.lorawanUplinkPeriodSec = static_cast<uint16_t>(min<uint32_t>(v, 65535U));
  }

  if (candidate.lorawanEnabled) {
    if (!hexField(candidate.lorawanDevEui, 16) ||
        (candidate.lorawanMode == 0 &&
         (!hexField(candidate.lorawanJoinEui, 16) || !hexField(candidate.lorawanAppKey, 32))) ||
        (candidate.lorawanMode == 1 &&
         (!hexField(candidate.lorawanNwkSKey, 32) || !hexField(candidate.lorawanAppSKey, 32)))) {
      server_.send(400, "text/plain", "invalid LoRaWAN credentials"); return;
    }
  }
  if (!candidate.validLoRaWAN()) {
    server_.send(400, "text/plain", "invalid LoRaWAN configuration"); return;
  }

  const RuntimeConfig previous = gConfig;
  const bool wasJoined = lorawan.isJoined();
  if (wasJoined) (void)lorawan.disconnect();
  gConfig = candidate;
  if (!gConfig.save()) {
    gConfig = previous;
    server_.send(503, "text/plain", "NVS save failed");
    return;
  }
  lorawan.setRegionalProfile(static_cast<RegionalProfile>(gConfig.lorawanRegion));
  if (!gConfig.lorawanEnabled) (void)lorawan.disconnect();
  auditConfigChange(previous, gConfig, "lorawan-web");
  server_.send(200, "text/plain", "LoRaWAN configuration saved");
}

void WebUi::handleLoRaWANUplink() {
  if (!rateLimit(lastMessageMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String text = server_.arg("text");
  const String raw = server_.arg("hex");
  uint8_t payload[Config::LORAWAN_MAX_PAYLOAD] = {};
  size_t len = 0;
  if (!raw.isEmpty()) {
    if ((raw.length() & 1U) || raw.length() > Config::LORAWAN_MAX_PAYLOAD * 2U) {
      server_.send(400, "text/plain", "invalid hex payload"); return;
    }
    auto n = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i < raw.length() / 2; ++i) {
      const int hi = n(raw[i * 2]), lo = n(raw[i * 2 + 1]);
      if (hi < 0 || lo < 0) { server_.send(400, "text/plain", "invalid hex payload"); return; }
      payload[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    len = raw.length() / 2;
  } else {
    if (text.isEmpty() || text.length() > Config::LORAWAN_MAX_PAYLOAD) {
      server_.send(400, "text/plain", "invalid text payload"); return;
    }
    memcpy(payload, text.c_str(), text.length());
    len = text.length();
  }
  const String confirmed = server_.arg("confirmed");
  const bool isConfirmed = confirmed == "1";
  const bool ok = lorawan.sendUplink(gConfig.lorawanFPort, payload, len, isConfirmed);
  server_.send(ok ? 202 : 409, "text/plain", ok ? "uplink queued" : "uplink rejected");
}

void WebUi::handleFiles() { const String dir = server_.arg("dir"); server_.send(200, "application/json", storage.listJson(dir.isEmpty() ? "/REC" : dir)); }


void WebUi::handleDownload() {
  const String path = server_.arg("path");
  if (!storage.isManagedAudioPath(path)) {
    server_.send(400, "text/plain", "invalid path");
    return;
  }
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    server_.send(404, "text/plain", "not found");
    return;
  }
  server_.sendHeader("Content-Disposition", "attachment; filename=\"" +
                     path.substring(path.lastIndexOf('/') + 1) + "\"");
  server_.streamFile(f, "audio/wav");
  f.close();
}

void WebUi::handleUpload() {
  HTTPUpload& up = server_.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadFailed_ = false;
    uploadBytes_ = 0;
    String name = up.filename;
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    uploadPath_ = "/REC/" + name;
    if (!storage.isManagedAudioPath(uploadPath_) ||
        up.totalSize > Config::WEB_UPLOAD_MAX_BYTES ||
        SD.exists(uploadPath_)) {
      uploadFailed_ = true;
      return;
    }
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (!spiLock.ok()) { uploadFailed_ = true; return; }
    uploadFile_ = SD.open(uploadPath_, FILE_WRITE);
    if (!uploadFile_) uploadFailed_ = true;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFailed_ || !uploadFile_ || up.currentSize > Config::WEB_UPLOAD_MAX_BYTES - uploadBytes_) {
      uploadFailed_ = true;
      return;
    }
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (!spiLock.ok() || uploadFile_.write(up.buf, up.currentSize) != up.currentSize) {
      uploadFailed_ = true;
      return;
    }
    uploadBytes_ += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile_) uploadFile_.close();
    if (up.status == UPLOAD_FILE_END && !uploadFailed_ &&
        (uploadBytes_ == 0 || uploadBytes_ > Config::WEB_UPLOAD_MAX_BYTES ||
         !isValidUploadedWav(uploadPath_))) {
      uploadFailed_ = true;
    }
    if (up.status == UPLOAD_FILE_ABORTED || uploadFailed_) {
      uploadFailed_ = true;
      if (!uploadPath_.isEmpty()) {
        SpiLock spiLock(pdMS_TO_TICKS(100));
        if (spiLock.ok()) SD.remove(uploadPath_);
      }
    }
  }
}

void WebUi::handleRename() {
  const String from = server_.arg("from");
  const String to = server_.arg("to");
  const bool ok = storage.renameFile(from, to);
  server_.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "FAIL");
}

void WebUi::handleMessages() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  const String query = server_.arg("q");
  uint64_t from = 0, to = UINT64_MAX;
  if (server_.hasArg("from")) from = strtoull(server_.arg("from").c_str(), nullptr, 10);
  if (server_.hasArg("to")) to = strtoull(server_.arg("to").c_str(), nullptr, 10);
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleMessageClear() {
  {
    StateLock lock(gState);
    if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
    for (auto& e : gState.messageHistory) e = MessageHistoryEntry{};
    gState.messageHistoryNext = 0;
    gState.messageHistoryCount = 0;
    gState.messageUnreadCount = 0;
  }
  (void)lora.persistMessageHistory();
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleMessageRead() {
  const String raw = server_.arg("ts");
  const bool all = raw == "all";
  if (!all && (raw.isEmpty() || raw.length() > 20)) {
    server_.send(400, "text/plain", "invalid timestamp"); return;
  }
  uint64_t ts = all ? 0 : strtoull(raw.c_str(), nullptr, 10);
  {
    StateLock lock(gState);
    if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
    for (auto& e : gState.messageHistory) {
      if (e.timestamp != 0 && (all || e.timestamp == ts) && !e.read) {
        e.read = true;
        if (gState.messageUnreadCount) --gState.messageUnreadCount;
      }
    }
  }
  (void)lora.persistMessageHistory();
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleMessageReply() {
  if (!rateLimit(lastMessageMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String target = server_.arg("source");
  const String text = server_.arg("plain");
  if (target.isEmpty() || target.length() > 10 || text.isEmpty() ||
      text.length() > Config::LORA_FRAGMENT_MAX_BYTES) {
    server_.send(400, "text/plain", "invalid reply"); return;
  }

  // Parse the source ID as an unsigned 32-bit value. String::toInt() is
  // signed on ESP32 and would reject/overflow valid IDs above INT32_MAX.
  uint32_t destination = 0;
  for (size_t i = 0; i < target.length(); ++i) {
    if (target[i] < '0' || target[i] > '9') {
      server_.send(400, "text/plain", "invalid source"); return;
    }
    const uint32_t digit = static_cast<uint32_t>(target[i] - '0');
    if (destination > (UINT32_MAX - digit) / 10U) {
      server_.send(400, "text/plain", "invalid source"); return;
    }
    destination = destination * 10U + digit;
  }
  if (destination == 0) {
    server_.send(400, "text/plain", "invalid source"); return;
  }

  // Use the protocol's destination field so the reply is unicast to the
  // selected source rather than broadcasting a "REPLY,<id>,..." text packet.
  const bool ok = lora.sendTextTo(destination, text);
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "FAIL");
}

void WebUi::handleMessageExport() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  const String format = server_.arg("format");
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
    server_.sendHeader("Content-Disposition", "attachment; filename=\"messages.csv\"");
    server_.send(200, "text/csv", out);
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
  server_.sendHeader("Content-Disposition", "attachment; filename=\"messages.json\"");
  server_.send(200, "application/json", out);
}

void WebUi::handleRadioHistory() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleRadioTune() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("freq");
  char* end = nullptr;
  const float freq = strtof(raw.c_str(), &end);
  if (!end || *end != '\0' || !isfinite(freq) ||
      freq < Config::LORA_MIN_FREQ_MHZ || freq > Config::LORA_MAX_FREQ_MHZ) {
    server_.send(400, "text/plain", "frequency outside configured legal band"); return;
  }
  const bool ok = lora.manualTune(freq);
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "TUNE FAILED");
}

void WebUi::handleStorageInfo() {
  const uint64_t total = storage.totalBytes();
  const uint64_t used = storage.usedBytes();
  if (!total) { server_.send(503, "text/plain", "storage unavailable"); return; }
  String j = "{\"total\":" + String(static_cast<unsigned long long>(total)) +
             ",\"used\":" + String(static_cast<unsigned long long>(used)) +
             ",\"free\":" + String(static_cast<unsigned long long>(total > used ? total-used : 0)) + "}";
  server_.send(200, "application/json", j);
}

void WebUi::handleChecksum() {
  const String path = server_.arg("path");
  if (!storage.isSafePath(path)) {
    server_.send(400, "text/plain", "invalid path"); return;
  }
  uint32_t crc = 0; uint64_t size = 0;
  if (!storage.checksumFile(path, crc, size)) {
    server_.send(404, "text/plain", "checksum failed"); return;
  }
  server_.send(200, "application/json",
               "{\"size\":" + String(static_cast<unsigned long long>(size)) +
               ",\"crc32\":\"" + String(crc, HEX) + "\"}");
}

void WebUi::handleSosHistory() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.send(200, "application/json", j);
}

void WebUi::handleLoraLog() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.send(200, "application/json", j);
}

void WebUi::handleHealthLog() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.send(200, "application/json", j);
}

void WebUi::handleBatteryCalibrate() {
  const String raw = server_.arg("voltage");
  char* end = nullptr;
  const float actual = strtof(raw.c_str(), &end);
  if (!end || *end != '\0' || !isfinite(actual) || actual < 2.5f || actual > 6.0f) {
    server_.send(400, "text/plain", "invalid voltage"); return;
  }
  float measured = NAN;
  {
    StateLock lock(gState);
    if (!lock.ok() || !gState.batteryAvailable || !isfinite(gState.batteryV)) {
      server_.send(409, "text/plain", "battery measurement unavailable"); return;
    }
    measured = gState.batteryV;
  }
  if (measured <= 0.1f) { server_.send(409, "text/plain", "invalid current measurement"); return; }
  RuntimeConfig candidate = gConfig;
  candidate.batteryCalibration *= actual / measured;
  if (!isfinite(candidate.batteryCalibration) ||
      candidate.batteryCalibration < 0.5f || candidate.batteryCalibration > 1.5f ||
      !candidate.save()) {
    server_.send(503, "text/plain", "calibration save failed"); return;
  }
  gConfig = candidate;
  server_.send(200, "text/plain", "Battery calibration saved: " +
               String(candidate.batteryCalibration, 5));
}

void WebUi::handleMessage() {
  if (!rateLimit(lastMessageMs_, Config::WEB_RATE_LIMIT_MS)) return;
  if (server_.contentLength() > Config::MAX_WEB_BODY) {
    server_.send(413, "text/plain", "payload too large");
    return;
  }
  String text = server_.arg("plain");
  if (text.isEmpty() || text.length() > Config::LORA_MAX_PACKET) {
    server_.send(400, "text/plain", "invalid payload"); return;
  }
  const bool ok = lora.sendText(text);
  const bool acked = lora.textAcked();
  server_.send(ok ? 200 : 503, "application/json",
               "{\"sent\":" + String(ok ? "true" : "false") +
               ",\"acked\":" + String(acked ? "true" : "false") + "}");
}

void WebUi::handleSos() {
  if (!rateLimit(lastSosMs_, Config::SOS_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("on");
  if (raw == "0") {
    if (!lora.cancelSOS()) {
      server_.send(503, "text/plain", "SOS cancel failed");
      return;
    }
    server_.send(200, "text/plain", "SOS OFF");
    return;
  }
  if (raw.isEmpty() || raw != "1") {
    server_.send(400, "text/plain", "invalid sos");
    return;
  }
  bool ok = lora.sendSOS();
  if (ok) {
    StateLock lock(gState);
    if (lock.ok()) gState.sos = true;
  }
  server_.send(ok ? 200 : 503, "text/plain", ok ? "SOS" : "FAIL");
}

void WebUi::handleSosStatus() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleScanStatus() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleScanStart() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String modeRaw = server_.arg("mode");
  const String dwellRaw = server_.arg("dwell");
  if (modeRaw != "1" && modeRaw != "2") {
    server_.send(400, "text/plain", "invalid mode"); return;
  }
  uint16_t dwell = Config::SCANNER_DEFAULT_DWELL_MS;
  if (!dwellRaw.isEmpty()) {
    for (size_t i = 0; i < dwellRaw.length(); ++i) {
      if (dwellRaw[i] < '0' || dwellRaw[i] > '9') {
        server_.send(400, "text/plain", "invalid dwell"); return;
      }
    }
    const long v = dwellRaw.toInt();
    if (v < static_cast<long>(Config::SCANNER_MIN_DWELL_MS) ||
        v > static_cast<long>(Config::SCANNER_MAX_DWELL_MS)) {
      server_.send(400, "text/plain", "dwell out of range"); return;
    }
    dwell = static_cast<uint16_t>(v);
  }
  const bool ok = lora.scannerStart(
      static_cast<uint8_t>(modeRaw == "2" ? 2 : 1), dwell);
  server_.send(ok ? 200 : 409, "text/plain", ok ? "SCAN" : "FAIL");
}

void WebUi::handleScanStop() {
  const bool ok = lora.scannerStop();
  server_.send(ok ? 200 : 409, "text/plain", ok ? "STOP" : "FAIL");
}

void WebUi::handleScanResults() {
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleHopSuggest() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  uint8_t suggested[Config::HOP_CHANNEL_MAX] = {};
  const size_t n = lora.scannerSuggestBestChannels(
      suggested, Config::HOP_CHANNEL_MAX);
  if (n == 0) {
    server_.send(409, "text/plain", "no scan results");
    return;
  }
  {
    StateLock lock(gState);
    if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleHopStatus() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleHopEnable() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") {
    server_.send(400, "text/plain", "invalid enable"); return;
  }
  const bool on = (raw == "1");
  {
    StateLock lock(gState);
    if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
    if (on && gState.hopChannelCount == 0) {
      server_.send(409, "text/plain", "no channels suggested"); return;
    }
    gState.hopEnabled = on;
  }
  server_.send(200, "text/plain", on ? "HOP ENABLED" : "HOP DISABLED");
}

void WebUi::handlePtt() {
  if (!rateLimit(lastPttMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") {
    server_.send(400, "text/plain", "invalid ptt");
    return;
  }
  const bool on = raw == "1";
  if (on) {
    if (!audio.startRecording()) {
      server_.send(503, "text/plain", "PTT recording start failed");
      return;
    }
  } else {
    if (!audio.stopRecording()) {
      server_.send(503, "text/plain", "PTT recording stop failed");
      return;
    }
  }

  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503); return; }
  gState.ptt = on;
  server_.send(200, "text/plain",
               on ? "PTT ON - recording" : "PTT OFF - recording stopped");
}

void WebUi::handleRecord() {
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") {
    server_.send(400, "text/plain", "invalid record");
    return;
  }
  const bool on = raw == "1";
  bool ok = on ? audio.startRecording() : audio.stopRecording();
  server_.send(ok ? 200 : 503, "text/plain", on ? "REC" : "STOP");
}

void WebUi::handlePlay() {
  String path = server_.arg("path");
  bool ok = audio.playFile(path);
  server_.send(ok ? 200 : 400, "text/plain", ok ? "PLAY" : "FAIL");
}

void WebUi::handleStop() {
  audio.stopPlayback();
  server_.send(200, "text/plain", "STOP");
}

void WebUi::handlePause() {
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") { server_.send(400, "text/plain", "invalid pause"); return; }
  const bool ok = audio.pausePlayback(raw == "1");
  server_.send(ok ? 200 : 409, "text/plain", ok ? "OK" : "FAIL");
}

void WebUi::handleSeek() {
  const String raw = server_.arg("ms");
  if (raw.isEmpty() || raw.length() > 10) { server_.send(400, "text/plain", "invalid seek"); return; }
  for (size_t i = 0; i < raw.length(); ++i)
    if (raw[i] < '0' || raw[i] > '9') { server_.send(400, "text/plain", "invalid seek"); return; }
  const uint32_t ms = raw.toInt();
  const bool ok = audio.seekPlaybackMs(ms);
  server_.send(ok ? 200 : 409, "text/plain", ok ? "OK" : "SEEK UNSUPPORTED");
}

void WebUi::handleQueue() {
  const String path = server_.arg("path");
  const bool ok = audio.enqueueFile(path);
  server_.send(ok ? 200 : 400, "text/plain", ok ? "QUEUED" : "QUEUE FAILED");
}

void WebUi::handleQueueClear() {
  audio.clearQueue();
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleRecordPause() {
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") { server_.send(400, "text/plain", "invalid record pause"); return; }
  const bool ok = audio.pauseRecording(raw == "1");
  server_.send(ok ? 200 : 409, "text/plain", ok ? "OK" : "FAIL");
}

void WebUi::handleRecordSplit() {
  const bool ok = audio.splitRecording();
  server_.send(ok ? 200 : 409, "text/plain", ok ? "SPLIT" : "FAIL");
}

void WebUi::handleVox() {
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") { server_.send(400, "text/plain", "invalid vox"); return; }
  float threshold = Config::VOX_THRESHOLD;
  uint32_t hang = Config::VOX_HANG_MS;
  if (server_.hasArg("threshold")) {
    char* end = nullptr;
    threshold = strtof(server_.arg("threshold").c_str(), &end);
    if (!end || *end != '\0' || !isfinite(threshold) || threshold < 0.01f || threshold > 1.0f) {
      server_.send(400, "text/plain", "invalid vox threshold"); return;
    }
  }
  if (server_.hasArg("hang")) {
    const String rawHang = server_.arg("hang");
    if (rawHang.isEmpty() || rawHang.length() > 5) {
      server_.send(400, "text/plain", "invalid vox hang"); return;
    }
    hang = static_cast<uint32_t>(rawHang.toInt());
    if (hang < 50 || hang > 5000) {
      server_.send(400, "text/plain", "invalid vox hang"); return;
    }
  }
  const bool ok = audio.setVox(raw == "1", threshold, hang);
  server_.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "FAIL");
}

void WebUi::handleRecordQuality() {
  const String level = server_.arg("level");
  uint8_t value = 2;
  if (level == "low") value = 0;
  else if (level == "medium") value = 1;
  else if (level == "high") value = 2;
  else {
    server_.send(400, "text/plain", "invalid record quality");
    return;
  }
  const bool ok = audio.setRecordQuality(value);
  server_.send(ok ? 200 : 409, "text/plain", ok ? "OK" : "recording active or save failed");
}

void WebUi::handleVad() {
  const String on = server_.arg("on");
  if (on != "0" && on != "1") {
    server_.send(400, "text/plain", "invalid vad"); return;
  }
  uint32_t adapt = 0;
  if (server_.hasArg("adapt")) {
    char* end = nullptr;
    const unsigned long v = strtoul(server_.arg("adapt").c_str(), &end, 10);
    if (!end || *end != '\0' || v > 60000UL) {
      server_.send(400, "text/plain", "invalid adapt"); return;
    }
    adapt = static_cast<uint32_t>(v);
  }
  const bool ok = audio.setVox(on == "1", server_.hasArg("threshold")
                                ? server_.arg("threshold").toFloat() : 0.08f,
                                server_.hasArg("hang") ? server_.arg("hang").toInt() : 700);
  if (ok && adapt) (void)audio.setVoxAdapt(adapt);
  server_.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "invalid vox");
}


void WebUi::handleUsbTransport() {
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") { server_.send(400, "text/plain", "invalid transport"); return; }
  const bool ok = audio.setUsbPlaybackTransport(raw == "1");
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "FAIL");
}

void WebUi::handleVolume() {
  const String raw = server_.arg("value");
  if (raw.isEmpty() || raw.length() > 3) {
    server_.send(400, "text/plain", "invalid volume");
    return;
  }
  for (size_t i = 0; i < raw.length(); ++i) {
    if (raw[i] < '0' || raw[i] > '9') {
      server_.send(400, "text/plain", "invalid volume");
      return;
    }
  }
  const int value = raw.toInt();
  if (value < 0 || value > 100) {
    server_.send(400, "text/plain", "invalid volume");
    return;
  }
  audio.setVolume(static_cast<uint8_t>(value));
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleDelete() {
  String p = server_.arg("path");
  bool ok = storage.removeFile(p);
  server_.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "FAIL");
}


void WebUi::handleMessagePersist() {
  const bool ok = lora.persistMessageHistory();
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "persist failed");
}

void WebUi::handleForwardStats() {
  String j = "{\"queued\":" + String(lora.forwardQueued()) +
             ",\"dropped\":" + String(lora.forwardDrops()) +
             ",\"lastDropMs\":" + String(lora.forwardLastDropMs()) +
             ",\"fragmentEvictions\":" + String(lora.fragmentEvictions()) +
             ",\"fragmentDrops\":" + String(lora.fragmentDrops()) +
             ",\"dutyBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyBudgetUs())) +
             ",\"dutyMaxBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyMaxBudgetUs())) +
             ",\"gzipStalls\":" + String(storage.gzipStalls()) + "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleAuthStats() {
  String j = "{\"failures\":" + String(authFailureWindowCount_) +
             ",\"blockedUntilMs\":" + String(authBlockedUntilMs_) +
             ",\"windowStartMs\":" + String(authFailureWindowStartMs_) +
             ",\"csrfFailures\":" + String(csrfFailures_) + "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

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

void WebUi::handleSensorNodes() {
  if (!rateLimit(lastSensorNodesMs_, Config::WEB_RATE_LIMIT_MS)) return;
  size_t count = 0;
  if (!bleSensorReader.sensorReader().snapshotNodes(gSensorSnapshots, SensorRegistry::MAX_SUPPORTED_NODES, count)) {
    server_.send(503, "application/json", "{\"ok\":false,\"error\":\"sensor snapshot unavailable\"}"); return;
  }
  String j = "{\"ok\":true,\"nodes\":[";
  for (size_t i = 0; i < count; ++i) { if (i) j += ','; j += sensorNodeJson(gSensorSnapshots[i].index, gSensorSnapshots[i].node, false); }
  server_.sendHeader("Cache-Control", "no-store"); server_.send(200, "application/json", j + "]}");
}

void WebUi::handleSensorNodeDetail() {
  if (!rateLimit(lastSensorNodesMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("id");
  if (raw.isEmpty()) { server_.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}"); return; }
  const long id = raw.toInt();
  if (id < 0 || id >= static_cast<long>(SensorRegistry::MAX_SUPPORTED_NODES)) { server_.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}"); return; }
  SensorRegistry::Node node{};
  if (!bleSensorReader.sensorReader().snapshotNode(static_cast<size_t>(id), node)) { server_.send(404, "application/json", "{\"ok\":false,\"error\":\"node not found\"}"); return; }
  server_.sendHeader("Cache-Control", "no-store"); server_.send(200, "application/json", "{\"ok\":true," + sensorNodeJson(static_cast<size_t>(id), node, true).substring(1));
}

void WebUi::handleSensorLive() {
  if (!rateLimit(lastSensorLiveMs_, Config::WEB_RATE_LIMIT_MS)) return;
  size_t count = 0;
  if (!bleSensorReader.sensorReader().snapshotNodes(gSensorSnapshots, SensorRegistry::MAX_SUPPORTED_NODES, count)) { server_.send(503, "application/json", "{\"ok\":false}"); return; }
  String j = "{\"ok\":true,\"nodes\":[";
  for (size_t i = 0; i < count; ++i) { if (i) j += ','; j += sensorNodeJson(gSensorSnapshots[i].index, gSensorSnapshots[i].node, true); }
  server_.sendHeader("Cache-Control", "no-store"); server_.send(200, "application/json", j + "]}");
}

bool parseSensorNodeId(ESPWebServerSecure& server, size_t& id) {
  const String raw = server.arg("id");
  if (raw.isEmpty()) return false;
  const long value = raw.toInt();
  if (value < 0 || value >= static_cast<long>(SensorRegistry::MAX_SUPPORTED_NODES)) return false;
  id = static_cast<size_t>(value); return true;
}

void WebUi::handleSensorForget() {
  if (!rateLimit(lastSensorActionMs_, Config::WEB_RATE_LIMIT_MS)) return;
  size_t id = 0;
  if (!parseSensorNodeId(server_, id)) { server_.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}"); return; }
  if (!bleSensorReader.sensorReader().requestForgetNode(id)) { server_.send(404, "application/json", "{\"ok\":false,\"error\":\"node not found\"}"); return; }
  server_.send(202, "application/json", "{\"ok\":true,\"queued\":true}");
}

void WebUi::handleSensorRefresh() {
  if (!rateLimit(lastSensorActionMs_, Config::WEB_RATE_LIMIT_MS)) return;
  size_t id = 0;
  if (!parseSensorNodeId(server_, id)) { server_.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}"); return; }
  if (!bleSensorReader.sensorReader().requestRefreshNode(id)) { server_.send(404, "application/json", "{\"ok\":false,\"error\":\"node not found\"}"); return; }
  server_.send(202, "application/json", "{\"ok\":true,\"queued\":true}");
}



void WebUi::handleMqttProvision() {
  static uint32_t lastMqttProvisionMs = 0;
  if (!rateLimit(lastMqttProvisionMs, Config::WEB_RATE_LIMIT_MS)) return;
  const String host = server_.arg("host");
  const String rawPort = server_.arg("port");
  const String user = server_.arg("user");
  const String pass = server_.arg("pass");
  if (host.isEmpty() || host.length() > 253 || rawPort.isEmpty() ||
      rawPort.length() > 5 || user.length() > 128 || pass.length() > 128) {
    server_.send(400, "application/json", "{"ok":false,"error":"invalid MQTT credentials"}");
    return;
  }
  uint32_t port = 0;
  for (size_t i = 0; i < rawPort.length(); ++i) {
    if (rawPort[i] < '0' || rawPort[i] > '9') {
      server_.send(400, "application/json", "{"ok":false,"error":"invalid MQTT port"}");
      return;
    }
    port = port * 10U + static_cast<uint32_t>(rawPort[i] - '0');
  }
  if (port == 0 || port > 65535U || host.indexOf('|') >= 0 ||
      user.indexOf('|') >= 0 || pass.indexOf('|') >= 0) {
    server_.send(400, "application/json", "{"ok":false,"error":"invalid MQTT credentials"}");
    return;
  }
#if defined(FIELDRADIO_PRODUCTION_BUILD)
  if (port == 1883) {
    server_.send(400, "application/json", "{"ok":false,"error":"plaintext MQTT disabled in production"}");
    return;
  }
#endif
  if (!mqtt.provisionCredentials(host, static_cast<uint16_t>(port), user, pass)) {
    server_.send(503, "application/json", "{"ok":false,"error":"MQTT provisioning failed"}");
    return;
  }
  server_.send(200, "application/json", "{"ok":true,"provisioned":true}");
}

void WebUi::handleMqttStatus() {
  static uint32_t lastMqttStatusMs = 0;
  if (!rateLimit(lastMqttStatusMs, Config::WEB_RATE_LIMIT_MS)) return;
  String j = "{"ok":true,"provisioned":";
  j += mqtt.credentialsProvisioned() ? "true" : "false";
  j += ","connected":";
  j += mqtt.isConnected() ? "true" : "false";
  j += ","passwordRotationWarning":";
  j += mqtt.passwordRotationWarning() ? "true" : "false";
  j += "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleNeighbors() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", lora.neighborsJson());
}

void WebUi::handleRoutes() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", lora.routesJson());
}

void WebUi::handleCaptureStart() {
  const String raw = server_.arg("duration");
  if (raw.isEmpty() || raw.length() > 8) {
    server_.send(400, "text/plain", "invalid duration"); return;
  }
  char* end = nullptr;
  const unsigned long ms = strtoul(raw.c_str(), &end, 10);
  if (!end || *end != '\0' || ms == 0 || ms > Config::CAPTURE_MAX_DURATION_MS) {
    server_.send(400, "text/plain", "invalid duration"); return;
  }
  const bool ok = lora.captureStart(static_cast<uint32_t>(ms));
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "capture unavailable");
}

void WebUi::handleCaptureStop() {
  const bool ok = lora.captureStop();
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "capture unavailable");
}

void WebUi::handleCaptureDump() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", lora.captureDumpJson());
}

void WebUi::handleAdr() {
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") {
    server_.send(400, "text/plain", "invalid adr"); return;
  }
  const bool ok = lora.setAdrEnabled(raw == "1");
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "ADR unavailable");
}

void WebUi::handleHopSync() {
  const String source = server_.arg("source");
  if (source != "gps" && source != "internal") {
    server_.send(400, "text/plain", "invalid source"); return;
  }
  lora.setHopSyncSource(source == "gps");
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleDedupStats() {
  String j = "{\"hits\":" + String(lora.dedupHits()) +
             ",\"misses\":" + String(lora.dedupMisses()) +
             ",\"cacheSize\":" + String(Config::LORA_DEDUP_CACHE_SIZE) +
             ",\"evictions\":" + String(lora.dedupEvictions()) + "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleTheme() {
  const String mode = server_.arg("mode");
  if (mode != "dark" && mode != "light") {
    server_.send(400, "text/plain", "invalid theme"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) {
    server_.send(503, "text/plain", "NVS unavailable"); return;
  }
  const bool ok = prefs.putString("theme", mode) > 0;
  prefs.end();
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "NVS save failed");
}

void WebUi::handleConfigMigrate() {
  const bool ok = gConfig.migrate();
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "migration failed");
}

void WebUi::handleNvs() {
  const String key = server_.arg("key");
  static const char* const allowed[] = {
    "cfgver", "freq", "bw", "sf", "cr", "sync", "power", "volume",
    "audsrc", "recqual", "batcal", "callsign", "theme", "bhealth_v", "bcycles", "bsamples"
  };
  bool allowedKey = false;
  for (const char* k : allowed)
    if (key == k) { allowedKey = true; break; }
  if (!allowedKey || key.length() > 15) {
    server_.send(400, "text/plain", "key not allowed"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", true)) {
    server_.send(503, "text/plain", "NVS unavailable"); return;
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
  server_.send(200, "application/json",
               "{\"key\":\"" + jsonEscape(key) + "\",\"value\":\"" + jsonEscape(value) + "\"}");
}

void WebUi::handleBatteryHistory() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}



void WebUi::handleMessageSchedule() {
  if (!server_.hasArg("at") || !server_.hasArg("text")) {
    server_.send(400, "text/plain", "at and text required"); return;
  }
  const String a = server_.arg("at");
  char* end = nullptr;
  const unsigned long long at = strtoull(a.c_str(), &end, 10);
  const String text = server_.arg("text");
  if (!end || *end != '\0' || at == 0 || text.isEmpty()) {
    server_.send(400, "text/plain", "invalid schedule"); return;
  }
  const bool ok = lora.scheduleMessage(static_cast<uint64_t>(at), text);
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "schedule full");
}

void WebUi::handleMessageScheduleList() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", lora.scheduledMessagesJson());
}

void WebUi::handleMessageScheduleDelete() {
  const String raw = server_.arg("id");
  if (raw.isEmpty() || raw.length() > 10) {
    server_.send(400, "text/plain", "invalid id"); return;
  }
  char* end = nullptr;
  const unsigned long value = strtoul(raw.c_str(), &end, 10);
  if (!end || *end != '\0' || value == 0 || value > UINT32_MAX) {
    server_.send(400, "text/plain", "invalid id"); return;
  }
  const uint32_t id = static_cast<uint32_t>(value);
  server_.send(lora.cancelScheduledMessage(id) ? 200 : 404,
               "text/plain", "OK");
}

void WebUi::handleRecordSchedule() {
  if (!server_.hasArg("start") || !server_.hasArg("duration")) {
    server_.send(400, "text/plain", "start and duration required"); return;
  }
  char* startEnd = nullptr;
  char* durationEnd = nullptr;
  const String rawStart = server_.arg("start");
  const String rawDuration = server_.arg("duration");
  if (rawStart.isEmpty() || rawStart.length() > 20 ||
      rawDuration.isEmpty() || rawDuration.length() > 10) {
    server_.send(400, "text/plain", "invalid schedule"); return;
  }
  const unsigned long long start = strtoull(rawStart.c_str(), &startEnd, 10);
  const unsigned long duration = strtoul(rawDuration.c_str(), &durationEnd, 10);
  if (!startEnd || *startEnd != '\0' ||
      !durationEnd || *durationEnd != '\0' ||
      start == 0 || duration == 0 || duration > Config::RECORD_MAX_SECONDS) {
    server_.send(400, "text/plain", "invalid schedule"); return;
  }
  recordScheduleStart_ = static_cast<uint64_t>(start);
  recordScheduleDurationSec_ = static_cast<uint32_t>(duration);
  recordScheduleActive_ = true;
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleRecordScheduleGet() {
  String j = "{\"active\":" + String(recordScheduleActive_ ? "true" : "false") +
             ",\"start\":" + String(static_cast<unsigned long long>(recordScheduleStart_)) +
             ",\"duration\":" + String(recordScheduleDurationSec_) + "}";
  server_.send(200, "application/json", j);
}

void WebUi::handleSosFormat() {
  const String raw = server_.arg("list");
  if (raw.isEmpty() || raw.length() > 32) {
    server_.send(400, "text/plain", "invalid format list"); return;
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
    else { server_.send(400, "text/plain", "unknown format"); return; }
    start = comma + 1;
  }
  server_.send(lora.setSosFormats(mask) ? 200 : 400, "text/plain", "OK");
}

void WebUi::handleSelfTest() {
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
  server_.send(200, "application/json", selfTestResult_);
}

void WebUi::handleSelfTestResult() {
  server_.send(200, "application/json",
               selfTestResult_.isEmpty() ? "{\"pass\":false,\"error\":\"not run\"}" : selfTestResult_);
}

void WebUi::handleLang() {
  const String code = server_.arg("set");
  if (code != "en" && code != "id") {
    server_.send(400, "text/plain", "invalid language"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) {
    server_.send(503, "text/plain", "NVS unavailable"); return;
  }
  const bool ok = prefs.putString("lang", code) > 0;
  prefs.end();
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "NVS save failed");
}

void WebUi::handleRfDetector() {
  const RfDetector& detector = lora.rfDetector();
  String j = "{\"forwardDbm\":" + String(detector.lastForwardDbm(), 2) +
             ",\"reflectedDbm\":" + String(detector.lastReflectedDbm(), 2) +
             ",\"vswr\":" + String(detector.lastVswr(), 2) +
             ",\"healthy\":" + String(detector.healthy() ? "true" : "false") + "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleRadioStats() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  const size_t n = gState.radioHistoryCount;
  if (n == 0) {
    server_.send(200, "application/json",
                 "{\"count\":0,\"rssiAvg\":null,\"rssiMin\":null,\"rssiMax\":null,\"snrAvg\":null,\"snrMin\":null,\"snrMax\":null}");
    return;
  }
  int32_t rssiSum = 0, rssiMin = 127, rssiMax = -127;
  double snrSum = 0.0, snrMin = 1000.0, snrMax = -1000.0;
  const size_t start = (gState.radioHistoryNext +
                        RuntimeState::RADIO_HISTORY_SIZE - n) %
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
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleRangeTest() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("on");
  if (raw != "0" && raw != "1") {
    server_.send(400, "text/plain", "invalid range-test");
    return;
  }
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  gState.rangeTest = raw == "1";
  server_.send(200, "text/plain", gState.rangeTest ? "RANGE TEST ON" : "RANGE TEST OFF");
}

void WebUi::handleHopSetChannels() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("list");
  if (raw.isEmpty() || raw.length() > 32) {
    server_.send(400, "text/plain", "invalid channel list"); return;
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
      server_.send(400, "text/plain", "invalid channel"); return;
    }
    int value = 0;
    for (size_t i = 0; i < token.length(); ++i) {
      if (token[i] < '0' || token[i] > '9') {
        server_.send(400, "text/plain", "invalid channel"); return;
      }
      value = value * 10 + (token[i] - '0');
    }
    if (value < 0 || value >= Config::HOP_CHANNEL_MAX) {
      server_.send(400, "text/plain", "channel out of range"); return;
    }
    for (size_t i = 0; i < count; ++i) {
      if (channels[i] == static_cast<uint8_t>(value)) {
        server_.send(400, "text/plain", "duplicate channel"); return;
      }
    }
    if (count >= Config::HOP_CHANNEL_MAX) {
      server_.send(400, "text/plain", "too many channels"); return;
    }
    channels[count++] = static_cast<uint8_t>(value);
    if (comma == raw.length()) break;
    start = comma + 1;
  }
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  for (size_t i = 0; i < Config::HOP_CHANNEL_MAX; ++i)
    gState.hopChannelList[i] = i < count ? channels[i] : 0;
  gState.hopChannelCount = static_cast<uint8_t>(count);
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleTrackPoints() {
  uint64_t from = 0, to = UINT64_MAX;
  if (server_.hasArg("from")) {
    const String raw = server_.arg("from");
    if (raw.isEmpty() || raw.length() > 20) {
      server_.send(400, "text/plain", "invalid from"); return;
    }
    char* end = nullptr;
    from = strtoull(raw.c_str(), &end, 10);
    if (!end || *end != '\0') {
      server_.send(400, "text/plain", "invalid from"); return;
    }
  }
  if (server_.hasArg("to")) {
    const String raw = server_.arg("to");
    if (raw.isEmpty() || raw.length() > 20) {
      server_.send(400, "text/plain", "invalid to"); return;
    }
    char* end = nullptr;
    to = strtoull(raw.c_str(), &end, 10);
    if (!end || *end != '\0') {
      server_.send(400, "text/plain", "invalid to"); return;
    }
  }
  size_t limit = 1000;
  if (server_.hasArg("limit")) {
    const String raw = server_.arg("limit");
    if (raw.isEmpty() || raw.length() > 4) {
      server_.send(400, "text/plain", "invalid limit"); return;
    }
    char* end = nullptr;
    const unsigned long v = strtoul(raw.c_str(), &end, 10);
    if (!end || *end != '\0' || v == 0) {
      server_.send(400, "text/plain", "invalid limit"); return;
    }
    limit = min<unsigned long>(v, 5000UL);
  }
  const String j = storage.readTrackCsv(from, to, limit);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}


void WebUi::handleTrackSimplified() {
  double epsilon = 10.0;
  if (server_.hasArg("epsilon")) {
    const String raw = server_.arg("epsilon");
    if (raw.isEmpty() || raw.length() > 12) {
      server_.send(400, "text/plain", "invalid epsilon"); return;
    }
    char* end = nullptr;
    epsilon = strtod(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(epsilon) || epsilon <= 0.0 || epsilon > 10000.0) {
      server_.send(400, "text/plain", "invalid epsilon"); return;
    }
  }
  const String j = storage.readTrackCsvSimplified(0, UINT64_MAX, 5000, epsilon);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleTrackDownload() {
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  File f = SD.open("/TRACK/TRACK.CSV", FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    server_.send(404, "text/plain", "track not found");
    return;
  }
  server_.sendHeader("Content-Disposition", "attachment; filename=TRACK.CSV");
  server_.streamFile(f, "text/csv");
  f.close();
}

void WebUi::handleTrack() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503); return; }
  String j = "{\"valid\":" + String(gState.gps.valid ? "true":"false") +
             ",\"lat\":" + String(gState.gps.lat,6) +
             ",\"lon\":" + String(gState.gps.lon,6) + "}";
  server_.send(200, "application/json", j);
}


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

void WebUi::handleConfig() {
  if (!rateLimit(lastConfigMs_, Config::WEB_RATE_LIMIT_MS)) return;
  RuntimeConfig candidate = gConfig;
  bool radioChanged = false;

  auto parseUnsigned = [](const String& raw, uint32_t maxValue, uint32_t& out) {
    if (raw.isEmpty() || raw.length() > 10) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < raw.length(); ++i) {
      if (raw[i] < '0' || raw[i] > '9') return false;
      const uint32_t digit = static_cast<uint32_t>(raw[i] - '0');
      if (value > (maxValue - digit) / 10U) return false;
      value = value * 10U + digit;
    }
    out = value;
    return true;
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

  if (server_.hasArg("freq")) {
    const String raw = server_.arg("freq");
    char* end = nullptr;
    const float value = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(value)) {
      server_.send(400, "text/plain", "invalid frequency"); return;
    }
    candidate.loraFreqMHz = value;
    radioChanged = true;
  }
  if (server_.hasArg("bw")) {
    const String raw = server_.arg("bw");
    char* end = nullptr;
    const float value = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(value)) {
      server_.send(400, "text/plain", "invalid bandwidth"); return;
    }
    candidate.loraBwKHz = value;
    radioChanged = true;
  }

  uint32_t value = 0;
  if (server_.hasArg("sf")) {
    if (!parseUnsigned(server_.arg("sf"), 12, value) || value < 5) {
      server_.send(400, "text/plain", "invalid spreading factor"); return;
    }
    candidate.loraSf = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (server_.hasArg("cr")) {
    if (!parseUnsigned(server_.arg("cr"), 8, value) || value < 5) {
      server_.send(400, "text/plain", "invalid coding rate"); return;
    }
    candidate.loraCr = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (server_.hasArg("sync")) {
    if (!parseUnsigned(server_.arg("sync"), 255, value)) {
      server_.send(400, "text/plain", "invalid sync word"); return;
    }
    candidate.loraSyncWord = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (server_.hasArg("power")) {
    if (!parseUnsigned(server_.arg("power"), 17, value) || value < 2) {
      server_.send(400, "text/plain", "invalid power"); return;
    }
    candidate.loraPowerDbm = static_cast<int8_t>(value);
    radioChanged = true;
  }
  if (server_.hasArg("volume")) {
    if (!parseUnsigned(server_.arg("volume"), 100, value)) {
      server_.send(400, "text/plain", "invalid volume"); return;
    }
    candidate.volume = static_cast<uint8_t>(value);
  }
  if (server_.hasArg("audio_source")) {
    if (!parseUnsigned(server_.arg("audio_source"), Config::AUDIO_SOURCE_USB, value)) {
      server_.send(400, "text/plain", "invalid audio source"); return;
    }
    candidate.audioRecordSource = static_cast<uint8_t>(value);
  }
  if (server_.hasArg("batcal")) {
    const String raw = server_.arg("batcal");
    char* end = nullptr;
    const float value = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(value) ||
        value < 0.5f || value > 1.5f) {
      server_.send(400, "text/plain", "invalid battery calibration"); return;
    }
    candidate.batteryCalibration = value;
  }
  if (server_.hasArg("callsign")) candidate.callsign = server_.arg("callsign");
  if (server_.hasArg("lora_key")) candidate.loraKeyHex = server_.arg("lora_key");
  if (server_.hasArg("ap_password")) candidate.apPassword = server_.arg("ap_password");
  if (server_.hasArg("web_password")) candidate.webPassword = server_.arg("web_password");
  if (server_.hasArg("ble_pairing")) {
    const String raw = server_.arg("ble_pairing");
    if (raw != "0" && raw != "1") {
      server_.send(400, "text/plain", "invalid BLE pairing setting"); return;
    }
    candidate.blePairingEnabled = raw == "1";
  }

  if (!candidate.validRadio() || candidate.volume > 100 ||
      candidate.audioRecordSource > Config::AUDIO_SOURCE_USB ||
      !validHex32(candidate.loraKeyHex) ||
      !validPassword(candidate.apPassword) ||
      (!candidate.webPassword.isEmpty() && !validPassword(candidate.webPassword)) ||
      candidate.apSsid.isEmpty() || candidate.webUser.isEmpty() ||
      !candidate.webPasswordConfigured() ||
      (!candidate.webPassword.isEmpty() && candidate.apPassword == candidate.webPassword)) {
    server_.send(400, "text/plain", "invalid configuration");
    return;
  }

  const RuntimeConfig previous = gConfig;
  const uint8_t previousSource = audio.recordSource();
  if (candidate.audioRecordSource != previousSource &&
      !audio.setRecordSource(candidate.audioRecordSource)) {
    server_.send(503, "text/plain", "Audio source is busy");
    return;
  }

  gConfig = candidate;
  if (radioChanged && !lora.applyConfig()) {
    gConfig = previous;
    (void)audio.setRecordSource(previousSource);
    server_.send(503, "text/plain", "LoRa configuration rejected by radio");
    return;
  }
  audio.setVolume(gConfig.volume);

  if (!gConfig.save()) {
    gConfig = previous;
    if (radioChanged) (void)lora.applyConfig();
    (void)audio.setRecordSource(previousSource);
    audio.setVolume(gConfig.volume);
    server_.send(503, "text/plain", "NVS save failed");
    return;
  }

  lora.updateSourceId();
  auditConfigChange(previous, gConfig, "web");
  server_.send(200, "text/plain",
               "Configuration saved; BLE pairing and WiFi credential changes apply after reboot");
}

void WebUi::handleConfigExport() {
  const RuntimeConfig& c = gConfig;
  String j = "{";
  j += "\"freq\":" + String(c.loraFreqMHz, 3);
  j += ",\"bw\":" + String(c.loraBwKHz, 3);
  j += ",\"sf\":" + String(c.loraSf) + ",\"cr\":" + String(c.loraCr);
  j += ",\"sync\":" + String(c.loraSyncWord) + ",\"power\":" + String(c.loraPowerDbm);
  j += ",\"volume\":" + String(c.volume) + ",\"audio_source\":" + String(c.audioRecordSource);
  j += ",\"record_quality\":" + String(c.audioRecordQuality);
  j += ",\"ble_pairing\":" + String(c.blePairingEnabled ? "true" : "false");
  j += ",\"battery_calibration\":" + String(c.batteryCalibration, 5);
  j += ",\"callsign\":\"" + jsonEscape(c.callsign) + "\"";
  // Secrets are deliberately omitted; exporting them into browser downloads is
  // an avoidable credential leak.
  j += "}";
  server_.sendHeader("Content-Disposition", "attachment; filename=\"fieldradio-config.json\"");
  server_.send(200, "application/json", j);
}

void WebUi::handleConfigBackup() {
  const String envelope = encryptConfigBackup();
  if (envelope.isEmpty()) {
    server_.send(503, "text/plain", "backup unavailable");
    return;
  }
  server_.sendHeader("Content-Disposition", "attachment; filename=\"fieldradio-config.frb\"");
  server_.send(200, "text/plain", envelope);
}

void WebUi::handleConfigRestore() {
  const String body = server_.arg("plain");
  if (body.length() < 16 || body.length() > 4096) {
    server_.send(400, "text/plain", "invalid backup");
    return;
  }
  String plain;
  if (!decryptConfigBackup(body, plain)) {
    server_.send(400, "text/plain", "backup authentication failed");
    return;
  }

  RuntimeConfig candidate = gConfig;
  bool seenFreq = false, seenBw = false, seenSf = false, seenCr = false;
  int pos = 0;
  while (pos <= static_cast<int>(plain.length())) {
    const int nl = plain.indexOf('\n', pos);
    const int end = nl < 0 ? plain.length() : nl;
    const String line = plain.substring(pos, end);
    String key, value;
    if (!line.isEmpty()) {
      if (!parseBackupLine(line, key, value)) {
        server_.send(400, "text/plain", "malformed backup");
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
      else if (key == "webuser") candidate.webUser = value;
      else if (key == "websalt") candidate.webPasswordSaltHex = value;
      else if (key == "webph") candidate.webPasswordHashHex = value;
      else { server_.send(400, "text/plain", "unknown backup key"); return; }
    }
    if (nl < 0) break;
    pos = nl + 1;
  }
  if (!seenFreq || !seenBw || !seenSf || !seenCr || !candidate.validRadio() ||
      candidate.volume > 100 || candidate.audioRecordQuality > 2 ||
      candidate.audioRecordSource > Config::AUDIO_SOURCE_USB ||
      !candidate.webPasswordConfigured()) {
    server_.send(400, "text/plain", "backup config invalid");
    return;
  }

  // Apply runtime state first. Persist only after every hardware-dependent
  // change succeeds, so a failed restore cannot leave NVS ahead of RAM.
  const RuntimeConfig previous = gConfig;
  const uint8_t previousSource = audio.recordSource();
  gConfig = candidate;
  if (!lora.applyConfig()) {
    gConfig = previous;
    (void)lora.applyConfig();
    server_.send(503, "text/plain", "radio restore failed");
    return;
  }
  if (!audio.setRecordQuality(gConfig.audioRecordQuality) ||
      !audio.setRecordSource(gConfig.audioRecordSource)) {
    gConfig = previous;
    (void)lora.applyConfig();
    (void)audio.setRecordQuality(previous.audioRecordQuality);
    (void)audio.setRecordSource(previousSource);
    audio.setVolume(previous.volume);
    (void)previous.save();
    server_.send(503, "text/plain", "audio restore failed");
    return;
  }
  audio.setVolume(gConfig.volume);

  if (!gConfig.save()) {
    gConfig = previous;
    (void)lora.applyConfig();
    (void)audio.setRecordQuality(previous.audioRecordQuality);
    (void)audio.setRecordSource(previousSource);
    audio.setVolume(previous.volume);
    (void)previous.save();
    server_.send(503, "text/plain", "NVS restore failed");
    return;
  }

  lora.updateSourceId();
  server_.send(200, "text/plain", "OK; reboot recommended");
}

void WebUi::handleChecksumSha256() {
  const String path = server_.arg("path");
  if (!storage.isSafePath(path)) { server_.send(400, "text/plain", "invalid path"); return; }
  String digest;
  uint64_t size = 0;
  if (!storage.sha256File(path, digest, size)) {
    server_.send(404, "text/plain", "checksum failed");
    return;
  }
  server_.send(200, "application/json", "{\"path\":\"" + jsonEscape(path) +
               "\",\"size\":" + String(static_cast<unsigned long long>(size)) +
               ",\"sha256\":\"" + digest + "\"}");
}


void WebUi::handleLogExport() {
  const String path = server_.arg("file");
  if (!storage.isSafePath(path) || !path.startsWith("/LOG/")) {
    server_.send(400, "text/plain", "invalid log path"); return;
  }
  if (server_.arg("format") != "gz") {
    server_.send(400, "text/plain", "only gz supported"); return;
  }
  const String outPath = "/LOG/.web-export.gz";
  if (!storage.exportGzip(path, outPath)) {
    server_.send(404, "text/plain", "compression failed"); return;
  }
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { server_.send(503, "text/plain", "storage busy"); return; }
  File f = SD.open(outPath, FILE_READ);
  if (!f) { server_.send(404, "text/plain", "export missing"); return; }
  server_.sendHeader("Content-Disposition", "attachment; filename=\"fieldradio-log.gz\"");
  server_.streamFile(f, "application/gzip");
  f.close();
  SD.remove(outPath);
}

void WebUi::handleFactoryReset() {
  if (server_.arg("confirm") != "RESET") {
    server_.send(400, "text/plain", "confirmation required");
    return;
  }
  // Freeze forward-queue persistence and wait for any in-flight LoRa operation
  // before touching /LORA. This prevents SD.remove()/rename() from racing a
  // concurrent FWD.Q rewrite.
  if (!lora.prepareForFactoryReset()) {
    server_.send(503, "text/plain", "LoRa storage busy");
    return;
  }

  // Erase the complete default NVS partition rather than only the application
  // namespace. With flash encryption/NVS encryption enabled this also removes
  // encrypted records at the storage layer; without encryption, physical
  // confidentiality cannot be guaranteed by software erase alone.
  {
    SpiLock spiLock(pdMS_TO_TICKS(500));
    if (!spiLock.ok()) {
      server_.send(503, "text/plain", "storage busy");
      return;
    }
    // Factory reset also removes user recordings/logs and routing artifacts.
    // Keep the managed directory structure itself so the next boot can reuse it.
    static const char* const managedDirs[] = {"/REC", "/LOG", "/TRACK", "/LORA"};
    for (const char* dir : managedDirs) {
      if (SD.exists(dir) && !eraseStorageTree(dir)) {
        lora.cancelFactoryReset();
        server_.send(503, "text/plain", "SD data erase failed");
        return;
      }
    }
  }

  const esp_err_t err = nvs_flash_erase();
  if (err != ESP_OK) {
    lora.cancelFactoryReset();
    server_.send(503, "text/plain", "NVS secure erase failed");
    return;
  }
  server_.send(200, "text/plain", "factory reset; rebooting");
  delay(100);
  ESP.restart();
}

void WebUi::handleAudioSource() {
  const String raw = server_.arg("source");
  if (raw != "0" && raw != "1" && raw != "2" && raw != "3") {
    server_.send(400, "text/plain", "invalid audio source");
    return;
  }

  const uint8_t source = static_cast<uint8_t>(raw.toInt());
  const uint8_t previous = gConfig.audioRecordSource;
  if (!audio.setRecordSource(source)) {
    server_.send(409, "text/plain", "audio source cannot change while recording/playback is active");
    return;
  }

  gConfig.audioRecordSource = source;
  if (!gConfig.save()) {
    (void)audio.setRecordSource(previous);
    gConfig.audioRecordSource = previous;
    server_.send(503, "text/plain", "audio source NVS save failed");
    return;
  }

  server_.send(200, "text/plain", "OK");
}

void WebUi::handleReboot() {
  server_.send(200, "text/plain", "rebooting");
  delay(100);
  ESP.restart();
}
