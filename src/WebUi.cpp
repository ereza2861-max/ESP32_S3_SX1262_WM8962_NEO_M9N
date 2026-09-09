#include "WebUi.h"
#include "Config.h"
#include "AppState.h"
#include "StorageManager.h"
#include "LoRaManager.h"
#include "AudioManager.h"

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
<button onclick="sos()">SOS</button><button onclick="rec(1)">REC</button>
<button onclick="rec(0)">STOP REC</button>
<button onclick="play()">PLAY</button><button onclick="stopPlay()">STOP</button>
</div>
<div class=card><input id=msg placeholder="LoRa message">
<button onclick="send()">Send</button></div>
<div class=card><input id=file value="/REC/">
<button onclick="play()">Play WAV</button></div>
<div class=card><h3>Status</h3><pre id=s></pre></div>
<div class=card><h3>Files</h3><pre id=f></pre></div>
<script>
async function j(u,o){let r=await fetch(u,o);return await r.text()}
async function refresh(){s.textContent=await j('/api/status');f.textContent=await j('/api/files')}
async function ptt(v){await j('/api/ptt?on='+v,{method:'POST'});refresh()}
async function sos(){await j('/api/sos',{method:'POST'});refresh()}
async function rec(v){await j('/api/record?on='+v,{method:'POST'});refresh()}
async function send(){await j('/api/message',{method:'POST',headers:{'Content-Type':'text/plain'},body:msg.value});refresh()}
async function play(){await j('/api/play?path='+encodeURIComponent(file.value),{method:'POST'});refresh()}
async function stopPlay(){await j('/api/stop',{method:'POST'});refresh()}
setInterval(refresh,2000);refresh()
</script></body></html>)HTML";

bool WebUi::auth() {
  if (!server_.authenticate(Config::WEB_USER, Config::WEB_PASSWORD)) {
    server_.requestAuthentication();
    return false;
  }
  return true;
}

void WebUi::begin() {
  server_.on("/", HTTP_GET, [this]{ if (auth()) handleRoot(); });
  server_.on("/api/status", HTTP_GET, [this]{ if (auth()) handleStatus(); });
  server_.on("/api/files", HTTP_GET, [this]{ if (auth()) handleFiles(); });
  server_.on("/api/message", HTTP_POST, [this]{ if (auth()) handleMessage(); });
  server_.on("/api/sos", HTTP_POST, [this]{ if (auth()) handleSos(); });
  server_.on("/api/ptt", HTTP_POST, [this]{ if (auth()) handlePtt(); });
  server_.on("/api/record", HTTP_POST, [this]{ if (auth()) handleRecord(); });
  server_.on("/api/play", HTTP_POST, [this]{ if (auth()) handlePlay(); });
  server_.on("/api/stop", HTTP_POST, [this]{ if (auth()) handleStop(); });
  server_.on("/api/delete", HTTP_POST, [this]{ if (auth()) handleDelete(); });
  server_.on("/api/track", HTTP_GET, [this]{ if (auth()) handleTrack(); });
  server_.on("/api/ota", HTTP_POST, [this]{ if (auth()) handleOta(); });
  server_.begin();
}

void WebUi::task() { server_.handleClient(); }

void WebUi::handleRoot() { server_.send_P(200, "text/html", INDEX_HTML); }

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
  j += "\"codec\":" + String(gState.codecReady ? "true":"false") + ",";
  j += "\"sd\":" + String(gState.storageReady ? "true":"false") + ",";
  j += "\"bt\":" + String(gState.btConnected ? "true":"false") + ",";
  j += "\"ptt\":" + String(gState.ptt ? "true":"false") + ",";
  j += "\"sos\":" + String(gState.sos ? "true":"false") + ",";
  j += "\"recording\":" + String(gState.recording ? "true":"false") + ",";
  j += "\"playing\":" + String(gState.playing ? "true":"false") + ",";
  j += "\"volume\":" + String(gState.volume) + ",";
  j += "\"tx\":" + String(gState.txPackets) + ",";
  j += "\"rx\":" + String(gState.rxPackets) + ",";
  j += "\"msg\":\"" + gState.lastMessage + "\"";
  j += "}";
  server_.send(200, "application/json", j);
}

void WebUi::handleFiles() { server_.send(200, "application/json", storage.listJson("/")); }

void WebUi::handleMessage() {
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
  bool ok = lora.sendSOS();
  if (ok) {
    StateLock lock(gState);
    if (lock.ok()) gState.sos = true;
  }
  server_.send(ok ? 200 : 503, "text/plain", ok ? "SOS" : "FAIL");
}

void WebUi::handlePtt() {
  bool on = server_.arg("on") == "1";
  StateLock lock(gState);
  if (!lock.ok()) { server_.send(503); return; }
  gState.ptt = on;
  server_.send(200, "text/plain", on ? "PTT ON" : "PTT OFF");
}

void WebUi::handleRecord() {
  bool on = server_.arg("on") == "1";
  bool ok = on ? audio.startRecording() : (audio.stopRecording(), true);
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

void WebUi::handleOta() {
  server_.send(Config::OTA_ENABLED ? 501 : 403, "text/plain",
               Config::OTA_ENABLED ? "OTA transport not implemented" : "OTA disabled");
}
