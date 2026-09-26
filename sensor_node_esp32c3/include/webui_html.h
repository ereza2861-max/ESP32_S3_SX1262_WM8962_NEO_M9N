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
<h1>FieldRadio Sensor OTA</h1>
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
async function refresh(){
  try{const r=await fetch('/status');const j=await r.json();
    status.textContent=JSON.stringify(j,null,2);
  }catch(e){status.textContent='status unavailable';}
}
refresh();setInterval(refresh,5000);
up.addEventListener('submit',()=>{result.textContent='uploading...';});
</script>
</body></html>)HTML";
