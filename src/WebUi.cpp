#include "WebUi.h"
#include "Config.h"
#include "AppState.h"
#include "StorageManager.h"
#include "LoRaManager.h"
#include "AudioManager.h"
#include "PersistentConfig.h"
#include <Update.h>
#include <esp_system.h>
#include <WiFi.h>

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
body{font-family:sans-serif;max-width:900px;margin:auto;padding:16px;background:#111;color:#eee}
button,input{font-size:1rem;margin:4px;padding:10px}pre{background:#222;padding:10px;overflow:auto}
.card{border:1px solid #444;border-radius:8px;padding:12px;margin:8px 0}
</style></head><body><h1>FieldRadio</h1>
<div class=card>
<button onclick="ptt(1)">PTT ON</button><button onclick="ptt(0)">PTT OFF</button>
<button onclick="sos(1)">SOS</button><button onclick="sos(0)">SOS OFF</button><button onclick="rec(1)">REC</button>
<button onclick="rec(0)">STOP REC</button><button onclick="recordPause(1)">PAUSE REC</button><button onclick="recordPause(0)">RESUME REC</button><button onclick="recordSplit()">SPLIT REC</button>
<label>Record source
<select id=audsrc onchange="setAudioSource()">
<option value="0">WM8960 MIC</option>
<option value="1">LINE-IN 2</option>
<option value="2">LINE-IN 3</option>
<option value="3">USB Audio</option>
</select></label>
<button onclick="play()">PLAY</button><button onclick="pausePlay(1)">PAUSE</button><button onclick="pausePlay(0)">RESUME</button><button onclick="stopPlay()">STOP</button><input id=seekms type=number value="0" min="0"><button onclick="seekPlay()">SEEK ms</button><button onclick="queue()">QUEUE</button><button onclick="clearQueue()">CLEAR QUEUE</button>
<button onclick="tone(880,120)">BEEP</button>
<label>USB monitor <input id=usbmon type=checkbox onchange="setUsbMonitor()"></label><label>SD→USB <input id=usbtransport type=checkbox onchange="setUsbTransport()"></label>
<label>Loopback <input id=loop type=checkbox onchange="setLoopback()"></label><label>AEC <input id=aec type=checkbox onchange="setAec()"></label><label>VOX <input id=vox type=checkbox onchange="setVox()"></label>
</div>
<div class=card><input id=msg placeholder="LoRa message">
<button onclick="send()">Send</button></div>
<div class=card><input id=file value="/REC/">
<button onclick="play()">Play WAV</button></div>
<div class=card><h3>Configuration</h3>
<input id=freq value="923" placeholder="Freq MHz"><input id=bw value="125" placeholder="BW kHz">
<input id=sf value="7" placeholder="SF"><input id=cr value="5" placeholder="CR 5-8">
<input id=pwr value="14" placeholder="Power dBm"><input id=sw value="18" placeholder="Sync word">
<input id=cs value="FIELD" placeholder="Callsign"><input id=key placeholder="LoRa AES-128 key (32 hex chars)"><input id=vol value="70" placeholder="Volume">
<input id=bat value="1.0" placeholder="Battery calibration">
<input id=aps value="" placeholder="AP password"><input id=wp value="" placeholder="Web password">
<button onclick="saveCfg()">Save config</button><button onclick="reboot()">Reboot</button></div>
<div class=card><h3>Status</h3><pre id=s></pre></div>
<div class=card><h3>Files</h3><pre id=f></pre></div>
<script>
async function j(u,o){let r=await fetch(u,o);return await r.text()}
async function refresh(){s.textContent=await j('/api/status');f.textContent=await j('/api/files')}
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
async function saveCfg(){
  const q=new URLSearchParams({freq:freq.value,bw:bw.value,sf:sf.value,cr:cr.value,
    power:pwr.value,sync:sw.value,callsign:cs.value,volume:vol.value,batcal:bat.value,
    audio_source:audsrc.value});
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
async function setVox(){await j('/api/vox?on='+(vox.checked?'1':'0'),{method:'POST'});refresh()}
async function syncSource(){
  try {
    const x=await (await fetch('/api/status')).json();
    if(x.audioSource!==undefined)audsrc.value=String(x.audioSource);
    if(x.usbMonitor!==undefined)usbmon.checked=!!x.usbMonitor;
    if(x.audioLoopback!==undefined)loop.checked=!!x.audioLoopback;
    if(x.usbPlaybackTransport!==undefined)usbtransport.checked=!!x.usbPlaybackTransport;
  } catch(e){}
}
setInterval(refresh,2000);syncSource();refresh()
</script></body></html>)HTML";

bool WebUi::sameOrigin() {
  const String origin = server_.header("Origin");
  if (origin.isEmpty()) return false;
  const String expected = String("http://") + WiFi.softAPIP().toString();
  return origin == expected;
}

bool WebUi::rateLimit(uint32_t& last, uint32_t interval) {
  const uint32_t now = millis();
  if (last != 0 && now - last < interval) {
    server_.send(429, "text/plain", "rate limited");
    return false;
  }
  last = now;
  return true;
}

bool WebUi::auth() {
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - authBlockedUntilMs_) < 0) {
    server_.send(429, "text/plain", "too many authentication failures");
    return false;
  }

  if (!server_.authenticate(gConfig.webUser.c_str(), gConfig.webPassword.c_str())) {
    if (now - authWindowStartMs_ >= 60000) {
      authWindowStartMs_ = now;
      authFailures_ = 0;
    }
    if (++authFailures_ >= 5) {
      authBlockedUntilMs_ = now + 30000;
      authFailures_ = 0;
    }
    server_.requestAuthentication();
    return false;
  }

  authFailures_ = 0;
  if (server_.method() == HTTP_POST && !sameOrigin()) {
    server_.send(403, "text/plain", "forbidden origin");
    return false;
  }
  return true;
}

void WebUi::begin() {
  static const char* const headerKeys[] = {"Origin", "Host"};
  server_.collectHeaders(headerKeys, 2);

  server_.on("/", HTTP_GET, [this]{ if (auth()) handleRoot(); });
  server_.on("/api/status", HTTP_GET, [this]{ if (auth()) handleStatus(); });
  server_.on("/api/files", HTTP_GET, [this]{ if (auth()) handleFiles(); });
  server_.on("/api/message", HTTP_POST, [this]{ if (auth()) handleMessage(); });
  server_.on("/api/sos", HTTP_POST, [this]{ if (auth()) handleSos(); });
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
  server_.on("/api/ota", HTTP_POST, [this]{ if (auth()) handleOta(); },
              [this]{ if (auth()) handleOtaUpload(); });
  server_.on("/api/reboot", HTTP_POST, [this]{ if (auth()) handleReboot(); });
  server_.on("/api/config", HTTP_POST, [this]{ if (auth()) handleConfig(); });
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
  server_.send_P(200, "text/html", INDEX_HTML);
}

void WebUi::handleStatus() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }

  String j = "{";
  j += "\"gps\":{\"valid\":" + String(gState.gps.valid ? "true":"false");
  j += ",\"lat\":" + String(gState.gps.lat,6);
  j += ",\"lon\":" + String(gState.gps.lon,6);
  j += ",\"alt\":" + String(gState.gps.alt,1);
  j += ",\"sat\":" + String(gState.gps.satellites) + "},";
  j += "\"lora\":" + String(gState.loraReady ? "true":"false") + ",";
  j += "\"rssi\":" + String(gState.loraRssi) + ",";
  j += "\"snr\":" + String(gState.loraSnr,1) + ",";
  j += "\"codec\":" + String(gState.codecReady ? "true":"false") + ",";
  j += "\"sd\":" + String(gState.storageReady ? "true":"false") + ",";
  j += "\"battery\":{\"available\":" + String(gState.batteryAvailable ? "true":"false");
  j += ",\"v\":";
  j += gState.batteryAvailable ? String(gState.batteryV, 2) : "null";
  j += ",\"low\":" + String(gState.batteryLow ? "true":"false");
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
  j += "\"tx\":" + String(gState.txPackets) + ",";
  j += "\"rx\":" + String(gState.rxPackets) + ",";
  j += "\"msg\":\"" + jsonEscape(gState.lastMessage) + "\",";
  j += "\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += "}";
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", j);
}

void WebUi::handleFiles() { server_.send(200, "application/json", storage.listJson("/REC")); }

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
  bool ok = lora.sendText(text);
  server_.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "FAIL");
}

void WebUi::handleSos() {
  if (!rateLimit(lastSosMs_, Config::SOS_RATE_LIMIT_MS)) return;
  const String raw = server_.arg("on");
  if (raw == "0") {
    StateLock lock(gState);
    if (!lock.ok()) { server_.send(503, "text/plain", "busy"); return; }
    gState.sos = false;
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
  const bool ok = audio.setVox(raw == "1", Config::VOX_THRESHOLD, Config::VOX_HANG_MS);
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

void WebUi::handleTrack() {
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503); return; }
  String j = "{\"valid\":" + String(gState.gps.valid ? "true":"false") +
             ",\"lat\":" + String(gState.gps.lat,6) +
             ",\"lon\":" + String(gState.gps.lon,6) + "}";
  server_.send(200, "application/json", j);
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
      candidate.webPassword.length() < 8 || candidate.apPassword.length() < 8 ||
      candidate.apPassword == candidate.webPassword) {
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

  server_.send(200, "text/plain",
               "Configuration saved; audio source updated; WiFi credential changes apply after reboot");
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

void WebUi::handleOta() {
  if (!Config::OTA_ENABLED) {
    server_.send(403, "text/plain", "OTA disabled");
    return;
  }
  if (Update.hasError()) {
    server_.send(500, "text/plain", "OTA failed");
    return;
  }
  server_.send(200, "text/plain", "OTA uploaded; rebooting");
  delay(100);
  ESP.restart();
}

void WebUi::handleOtaUpload() {
  if (!Config::OTA_ENABLED) return;
  HTTPUpload& upload = server_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (gState.ptt || gState.recording || gState.playing) {
      Update.abort();
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.abort();
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.abort();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

void WebUi::handleReboot() {
  server_.send(200, "text/plain", "rebooting");
  delay(100);
  ESP.restart();
}
