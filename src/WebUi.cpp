#include "WebUi.h"
#include "Config.h"
#include "AppState.h"
#include "StorageManager.h"
#include "LoRaManager.h"
#include "AudioManager.h"
#include "PersistentConfig.h"
#include <cstring>
#include <WiFi.h>
#include <SD.h>
#include <Preferences.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#ifndef CONFIG_SECURE_BOOT_V2_ENABLED
#define CONFIG_SECURE_BOOT_V2_ENABLED 0
#endif
#ifndef CONFIG_SECURE_FLASH_ENC_ENABLED
#define CONFIG_SECURE_FLASH_ENC_ENABLED 0
#endif
#include <esp_system.h>
#include <nvs_flash.h>

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

extern StorageManager storage;
extern LoRaManager lora;
extern AudioManager audio;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>FieldRadio</title><style>
body{font-family:sans-serif;max-width:1100px;margin:auto;padding:16px;background:#111;color:#eee}
body.light{background:#f5f5f5;color:#111} body.light .card{border-color:#bbb} body.light pre{background:#e8e8e8}
.card{border:1px solid #444;border-radius:8px;padding:12px;margin:8px 0}
@media(max-width:600px){body{padding:8px}.card{padding:8px}button,input,select{width:100%;box-sizing:border-box;margin:3px 0}table{font-size:.8rem;display:block;overflow-x:auto}}
button,input{font-size:1rem;margin:4px;padding:10px}pre{background:#222;padding:10px;overflow:auto}
.battery-low{outline:3px solid orange}.battery-critical{outline:4px solid red}
</style></head><body><h1>FieldRadio</h1>
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
<label>Loopback <input id=loop type=checkbox onchange="setLoopback()"></label><label>VOX threshold <input id=voxThreshold type=number step="0.01" min="0.01" max="1" value="0.08"></label><label>hang ms <input id=voxHang type=number min="50" max="5000" value="700"></label><label>AEC <input id=aec type=checkbox onchange="setAec()"></label><label>VOX <input id=vox type=checkbox onchange="setVox()"></label>
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
<div class=card><h3>Configuration</h3>
<input id=freq value="923" placeholder="Freq MHz"><input id=bw value="125" placeholder="BW kHz">
<input id=sf value="7" placeholder="SF"><input id=cr value="5" placeholder="CR 5-8">
<input id=pwr value="14" placeholder="Power dBm"><input id=sw value="18" placeholder="Sync word">
<input id=cs value="FIELD" placeholder="Callsign"><input id=key placeholder="LoRa AES-128 key (32 hex chars)"><input id=vol value="70" placeholder="Volume">
<input id=bat value="1.0" placeholder="Battery calibration">
<input id=batActual placeholder="Actual battery voltage, e.g. 3.95"><button onclick="calBattery()">CALIBRATE BATTERY</button>
<input id=aps value="" placeholder="AP password"><input id=wp value="" placeholder="Web password">
<button onclick="saveCfg()">Save config</button><button onclick="reboot()">Reboot</button></div>
<div class=card><h3>Radio diagnostics</h3>
<input id=tuneFreq type=number step="0.001" min="920" max="923" placeholder="Frequency MHz">
<button onclick="tuneRadio()">Manual tune</button><button onclick="refreshRadioHistory()">Refresh RSSI/SNR history</button>
<pre id=radioHistory></pre><div id=storageInfo></div></div>
<div class=card><h3>Channel Scanner</h3>
<button onclick="scanStart(1)">Scan Once</button>
<button onclick="scanStart(2)">Scan Continuous</button>
<button onclick="scanStop()">Stop Scan</button>
<button onclick="hopSuggest()">Suggest Hop Channels</button>
<button onclick="hopEnable(1)">Enable Hop</button>
<button onclick="hopEnable(0)">Disable Hop</button>
<pre id=scanmsg></pre>
<div id=scanresults><table><thead><tr>
<th>Freq MHz</th><th>RSSI avg</th><th>RSSI peak</th><th>SNR</th>
<th>Occupancy</th><th>Preamble</th><th>Load</th>
</tr></thead><tbody id=scantbody></tbody></table></div>
<div id=hopsummary></div>
</div>
<div class=card><button onclick="toggleTheme()">Dark/light</button><span id=toast></span></div><div class=card><h3>Status</h3><pre id=s></pre></div>
<div class=card><h3>Files</h3><input id=fileDir value="/REC/"><button onclick="refreshFiles()">Open folder</button><pre id=f></pre>
<input id=upfile type=file accept=".wav,.WAV"><button onclick="uploadFile()">UPLOAD WAV</button>
<input id=renameFrom placeholder="/REC/old.WAV"><input id=renameTo placeholder="/REC/new.WAV"><button onclick="renameFile()">RENAME</button>
<a id=trackDownload href="/api/track/download">Download GPS track</a>
</div>
<script>
const CSRF_TOKEN='__CSRF_TOKEN__';
async function j(u,o={}){o.headers=Object.assign({},o.headers||{},o.method&&o.method.toUpperCase()!=='GET'?{'X-CSRF-Token':CSRF_TOKEN}:{});let r=await fetch(u,o);return await r.text()}
function toast(t){document.getElementById('toast').textContent=t;setTimeout(()=>document.getElementById('toast').textContent='',2500)}
function toggleTheme(){document.body.classList.toggle('light');localStorage.setItem('fieldradio-theme',document.body.classList.contains('light')?'light':'dark')}
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
async function refreshRadioHistory(){
 try{const a=await (await fetch('/api/radio/history')).json();radioHistory.textContent=a.map(x=>`+${x.ms}ms RSSI=${x.rssi} SNR=${x.snr}`).join('\\n');
 const st=await (await fetch('/api/storage/info')).json();storageInfo.textContent=`SD used ${st.used} / ${st.total} bytes (${st.free} free)`;
 }catch(e){toast('Radio/storage refresh failed')}
}
async function refreshSosHistory(){try{const a=await (await fetch('/api/sos-history')).json();sosBadge.textContent=a.map(x=>`event=${x.event} seq=${x.seq} peer=${x.peer}`).join(' | ')}catch(e){}}
async function refreshFiles(){try{f.textContent=await j('/api/files?dir='+encodeURIComponent(fileDir.value))}catch(e){toast('File list failed')}}
async function refresh(){
  const raw=await j('/api/status');s.textContent=raw;await refreshFiles();
  try{const x=JSON.parse(raw);document.getElementById('unreadBadge').textContent=(x.messageUnread||0)+' unread';document.getElementById('sosBadge').textContent=x.sosEscalated?'SOS ESCALATED':(x.sos?'SOS ACTIVE':'');document.body.classList.toggle('battery-low',!!x.battery?.low);document.body.classList.toggle('battery-critical',!!x.battery?.critical)}catch(e){}
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
setInterval(refresh,1000);syncSource();refresh();refreshMessages();refreshRadioHistory();refreshSosHistory()
setInterval(refreshScan,3000);setInterval(refreshHop,3000);setInterval(refreshMessages,2000);setInterval(refreshRadioHistory,3000);refreshScan();refreshHop()
</script></body></html>)HTML";

bool WebUi::sameOrigin() {
  const String origin = server_.header("Origin");
  if (origin.isEmpty()) return false;
  const String expected = String("http://") + WiFi.softAPIP().toString();
  return origin == expected;
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
  if (server_.method() != HTTP_POST || csrfTokenHex_.isEmpty()) return false;
  const String supplied = server_.header("X-CSRF-Token");
  if (supplied.length() != csrfTokenHex_.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < supplied.length(); ++i)
    diff |= static_cast<uint8_t>(supplied[i] ^ csrfTokenHex_[i]);
  return diff == 0;
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
      "; HttpOnly; SameSite=Strict");

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
      auditAuth(false);
      if (authFailures_ >= 5) {
        authBlockedUntilMs_ = now + 30000;
        authFailures_ = 0;
      }
      server_.requestAuthentication();
      return false;
    }
    authFailures_ = 0;
    if (!issueSession()) {
      auditAuth(false);
      server_.send(503, "text/plain", "session initialization failed");
      return false;
    }
    auditAuth(true);
  }
  if (server_.method() == HTTP_POST) {
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
  static const char* const headerKeys[] = {
      "Origin", "Host", "Cookie", "X-CSRF-Token", "Authorization"};
  server_.collectHeaders(headerKeys, 5);

  server_.on("/", HTTP_GET, [this]{ if (auth()) handleRoot(); });
  server_.on("/api/status", HTTP_GET, [this]{ if (auth()) handleStatus(); });
  server_.on("/api/v1/status", HTTP_GET, [this]{ if (auth()) handleStatus(); });
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
  server_.on("/api/radio/history", HTTP_GET, [this]{ if (auth()) handleRadioHistory(); });
  server_.on("/api/radio/tune", HTTP_POST, [this]{ if (auth()) handleRadioTune(); });
  server_.on("/api/storage/info", HTTP_GET, [this]{ if (auth()) handleStorageInfo(); });
  server_.on("/api/storage/checksum", HTTP_GET, [this]{ if (auth()) handleChecksum(); });
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
  server_.on("/api/ptt", HTTP_POST, [this]{ if (auth()) handlePtt(); });
  server_.on("/api/record", HTTP_POST, [this]{ if (auth()) handleRecord(); });
  server_.on("/api/play", HTTP_POST, [this]{ if (auth()) handlePlay(); });
  server_.on("/api/stop", HTTP_POST, [this]{ if (auth()) handleStop(); });
  server_.on("/api/pause", HTTP_POST, [this]{ if (auth()) handlePause(); });
  server_.on("/api/seek", HTTP_POST, [this]{ if (auth()) handleSeek(); });
  server_.on("/api/queue", HTTP_POST, [this]{ if (auth()) handleQueue(); });
  server_.on("/api/queue-clear", HTTP_POST, [this]{ if (auth()) handleQueueClear(); });
  server_.on("/api/record-pause", HTTP_POST, [this]{ if (auth()) handleRecordPause(); });
  server_.on("/api/record-split", HTTP_POST, [this]{ if (auth()) handleRecordSplit(); });
  server_.on("/api/vox", HTTP_POST, [this]{ if (auth()) handleVox(); });
  server_.on("/api/usb-transport", HTTP_POST, [this]{ if (auth()) handleUsbTransport(); });
  server_.on("/api/volume", HTTP_POST, [this]{ if (auth()) handleVolume(); });
  server_.on("/api/delete", HTTP_POST, [this]{ if (auth()) handleDelete(); });
  server_.on("/api/track", HTTP_GET, [this]{ if (auth()) handleTrack(); });
  server_.on("/api/track/download", HTTP_GET, [this]{ if (auth()) handleTrackDownload(); });
  server_.on("/api/reboot", HTTP_POST, [this]{ if (auth()) handleReboot(); });
  server_.on("/api/config", HTTP_POST, [this]{ if (auth()) handleConfig(); });
  server_.on("/api/config/export", HTTP_GET, [this]{ if (auth()) handleConfigExport(); });
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
  server_.begin();
}

void WebUi::task() { server_.handleClient(); }

void WebUi::handleRoot() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("X-Content-Type-Options", "nosniff");
  server_.sendHeader("X-Frame-Options", "DENY");
  server_.sendHeader("Referrer-Policy", "no-referrer");
  server_.sendHeader("Content-Security-Policy",
                     "default-src 'self'; script-src 'unsafe-inline'; "
                     "style-src 'unsafe-inline'; object-src 'none'; "
                     "base-uri 'none'; frame-ancestors 'none'");
  String page = FPSTR(INDEX_HTML);
  page.replace("__CSRF_TOKEN__", csrfTokenHex_);
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
  j += "\"lora\":" + String(gState.loraReady ? "true":"false") + ",";
  j += "\"rssi\":" + String(gState.loraRssi) + ",";
  j += "\"snr\":" + String(gState.loraSnr,1) + ",";
  j += "\"codec\":" + String(gState.codecReady ? "true":"false") + ",";
  j += "\"sd\":" + String(gState.storageReady ? "true":"false") + ",";
  j += "\"battery\":{\"available\":" + String(gState.batteryAvailable ? "true":"false");
  j += ",\"v\":";
  j += gState.batteryAvailable ? String(gState.batteryV, 2) : "null";
  j += ",\"low\":" + String(gState.batteryLow ? "true":"false");
  j += ",\"percent\":" + String(gState.batteryPercent);
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
       ",\"channelOccupancy\":" + String(gState.channelOccupancy) + "},";
  j += "\"tx\":" + String(gState.txPackets) + ",";
  j += "\"rx\":" + String(gState.rxPackets) + ",";
  j += "\"msg\":\"" + jsonEscape(gState.lastMessage) + "\",";
  j += "\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
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
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  for (auto& e : gState.messageHistory) e = MessageHistoryEntry{};
  gState.messageHistoryNext = 0;
  gState.messageHistoryCount = 0;
  gState.messageUnreadCount = 0;
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleMessageRead() {
  const String raw = server_.arg("ts");
  const bool all = raw == "all";
  if (!all && (raw.isEmpty() || raw.length() > 20)) {
    server_.send(400, "text/plain", "invalid timestamp"); return;
  }
  uint64_t ts = all ? 0 : strtoull(raw.c_str(), nullptr, 10);
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
  for (auto& e : gState.messageHistory) {
    if (e.timestamp != 0 && (all || e.timestamp == ts) && !e.read) {
      e.read = true;
      if (gState.messageUnreadCount) --gState.messageUnreadCount;
    }
  }
  server_.send(200, "text/plain", "OK");
}

void WebUi::handleMessageReply() {
  if (!rateLimit(lastMessageMs_, Config::WEB_RATE_LIMIT_MS)) return;
  const String target = server_.arg("source");
  const String text = server_.arg("plain");
  if (target.isEmpty() || target.length() > 10 || text.isEmpty() ||
      text.length() > Config::LORA_MAX_PACKET - 16) {
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
         ",\"webStack\":" + String(e.webStackMin) + "}";
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

  if (!candidate.validRadio() || candidate.volume > 100 ||
      candidate.audioRecordSource > Config::AUDIO_SOURCE_USB ||
      candidate.loraKeyHex.length() != 32 ||
      candidate.apPassword.length() > 63 || candidate.webPassword.length() > 63 ||
      candidate.apSsid.isEmpty() || candidate.webUser.isEmpty() ||
      candidate.apPassword.length() < 8 || !candidate.webPasswordConfigured() ||
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

  auditConfigChange(previous, gConfig, "web");
  server_.send(200, "text/plain",
               "Configuration saved; audio source updated; WiFi credential changes apply after reboot");
}

void WebUi::handleConfigExport() {
  const RuntimeConfig& c = gConfig;
  String j = "{";
  j += "\"freq\":" + String(c.loraFreqMHz, 3);
  j += ",\"bw\":" + String(c.loraBwKHz, 3);
  j += ",\"sf\":" + String(c.loraSf) + ",\"cr\":" + String(c.loraCr);
  j += ",\"sync\":" + String(c.loraSyncWord) + ",\"power\":" + String(c.loraPowerDbm);
  j += ",\"volume\":" + String(c.volume) + ",\"audio_source\":" + String(c.audioRecordSource);
  j += ",\"battery_calibration\":" + String(c.batteryCalibration, 5);
  j += ",\"callsign\":\"" + jsonEscape(c.callsign) + "\"";
  // Secrets are deliberately omitted; exporting them into browser downloads is
  // an avoidable credential leak.
  j += "}";
  server_.sendHeader("Content-Disposition", "attachment; filename=\"fieldradio-config.json\"");
  server_.send(200, "application/json", j);
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

void WebUi::handleFactoryReset() {
  if (server_.arg("confirm") != "RESET") {
    server_.send(400, "text/plain", "confirmation required");
    return;
  }
  // Erase the complete default NVS partition rather than only the application
  // namespace. With flash encryption/NVS encryption enabled this also removes
  // encrypted records at the storage layer; without encryption, physical
  // confidentiality cannot be guaranteed by software erase alone.
  const esp_err_t err = nvs_flash_erase();
  if (err != ESP_OK) {
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
