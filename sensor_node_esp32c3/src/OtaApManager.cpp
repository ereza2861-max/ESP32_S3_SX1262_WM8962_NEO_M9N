#include "OtaApManager.h"

#ifndef SENSOR_NODE_ESP32C3
#error "OtaApManager.cpp is ESP32-C3-only"
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <Update.h>
#include "webui_html.h"

namespace {
constexpr char NVS_NAMESPACE[] = "ota";
constexpr char NVS_KEY_AP_ENABLED[] = "ap_enabled";
constexpr uint16_t AP_PORT = 80;
constexpr const char* AP_SSID_PREFIX = "FieldRadio-Sensor-";

WebServer* gServer = nullptr;
OtaApManager* gManager = nullptr;

// ArduinoOTA event handlers forward to a global manager pointer so the
// callbacks stay in OtaApManager while the ArduinoOTA API uses free functions.
void onOtaStart() {
  Serial.println("OTA: ArduinoOTA update started");
  if (gManager) {
    // Keep the AP alive during the transfer regardless of the timer.
    // (The timer is checked in task(); a client-connected state extends it.)
  }
}
void onOtaEnd() { Serial.println("OTA: ArduinoOTA update complete"); }
void onOtaProgress(unsigned int p, unsigned int t) {
  if (p == t || (p % 10 == 0)) Serial.printf("OTA: %u/%u\n", p, t);
}
void onOtaError(ota_error_t e) {
  Serial.printf("OTA: ArduinoOTA error %u\n", static_cast<unsigned>(e));
}

}  // namespace

bool OtaApManager::begin() {
  gManager = this;
  loadState();

  if (!otaPassword_.isEmpty() && otaPassword_.length() < 12) {
    Serial.println("WARN: OTA password too short; AP stays disabled");
    otaPassword_ = "";
  }

  const bool requestedApAtBoot = apEnabled_;
  // loadState() stores the desired state, while startAp() uses apEnabled_
  // as the runtime "AP is actually up" guard. Clear the runtime flag before
  // starting so a persisted enabled state does not make startAp() return
  // without bringing the AP up.
  apEnabled_ = false;

  if (requestedApAtBoot && !otaPassword_.isEmpty()) {
    if (!startAp()) {
      Serial.println("WARN: AP start failed at boot; staying disabled");
      apEnabled_ = false;
      saveState();
    }
  }
  return true;
}

void OtaApManager::loadState() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  apEnabled_ = prefs.getBool(NVS_KEY_AP_ENABLED, false);
  otaPassword_ = prefs.getString("opass", "");
  prefs.end();
}

void OtaApManager::saveState() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBool(NVS_KEY_AP_ENABLED, apEnabled_);
  prefs.end();
}

bool OtaApManager::startAp() {
  if (apEnabled_) return true;
  const String ssid = String(AP_SSID_PREFIX) + String((uint32_t)ESP.getEfuseMac(), HEX);

  WiFi.mode(WIFI_AP);
  // OPEN AP — deliberate security trade-off, documented in header.
  if (!WiFi.softAP(ssid.c_str())) {
    Serial.println("OTA: softAP start failed");
    return false;
  }
  delay(100);

  if (!gServer) gServer = new WebServer(AP_PORT);
  if (!gServer) return false;

  gServer->on("/", HTTP_GET, [this]() { handleRoot(); });
  gServer->on("/status", HTTP_GET, [this]() { handleStatus(); });
  gServer->on("/config", HTTP_POST, [this]() { handleConfig(); });
  gServer->on("/update", HTTP_POST,
              [this]() { handleUploadDone(); },
              [this]() { handleUpload(); });
  gServer->begin();

  ArduinoOTA.setHostname(ssid.c_str());
  ArduinoOTA.setPassword(otaPassword_.c_str());
  ArduinoOTA.onStart(onOtaStart);
  ArduinoOTA.onEnd(onOtaEnd);
  ArduinoOTA.onProgress(onOtaProgress);
  ArduinoOTA.onError(onOtaError);
  ArduinoOTA.begin();

  apEnabled_ = true;
  apStartedAtMs_ = millis();
  lastClientSeenMs_ = apStartedAtMs_;
  Serial.printf("OTA: AP up ssid=%s ip=%s window=%lus\n",
                ssid.c_str(),
                WiFi.softAPIP().toString().c_str(),
                static_cast<unsigned long>(OTA_AP_WINDOW_MS / 1000));
  return true;
}

void OtaApManager::stopAp() {
  if (!apEnabled_) return;
  ArduinoOTA.end();
  if (gServer) { gServer->stop(); delete gServer; gServer = nullptr; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apEnabled_ = false;
  clientConnected_ = false;
  Serial.println("OTA: AP down");
  saveState();
}

void OtaApManager::toggleAp() {
  if (apEnabled_) {
    stopAp();
  } else {
    if (otaPassword_.isEmpty()) {
      Serial.println("WARN: cannot enable AP: no OTA password provisioned");
      return;
    }
    startAp();
    saveState();
  }
}

void OtaApManager::task() {
  if (!apEnabled_) return;
  const uint32_t now = millis();

  if (gServer) gServer->handleClient();
  ArduinoOTA.handle();

  // Track the most recent client to extend the auto-shutdown window.
  const int stationCount = WiFi.softAPgetStationNum();
  clientConnected_ = stationCount > 0;
  if (clientConnected_) lastClientSeenMs_ = now;

  // Auto-shutdown: window from apStartedAtMs_, extended while a client is
  // connected. Only shuts down when BOTH the base window and the client
  // grace window have expired.
  const bool baseWindowExpired =
      (now - apStartedAtMs_) >= OTA_AP_WINDOW_MS;
  const bool clientGraceExpired =
      (now - lastClientSeenMs_) >= OTA_AP_WINDOW_MS;
  if (baseWindowExpired && !clientConnected_ && clientGraceExpired) {
    Serial.println("OTA: provisioning window expired; stopping AP");
    stopAp();
  }
}

bool OtaApManager::checkOtaPassword(const String& supplied) {
  if (otaPassword_.isEmpty()) return false;
  if (supplied.length() != otaPassword_.length()) return false;
  // Constant-time compare.
  uint8_t diff = 0;
  for (size_t i = 0; i < supplied.length(); ++i)
    diff |= (uint8_t)(supplied[i] ^ otaPassword_[i]);
  return diff == 0;
}

void OtaApManager::handleRoot() {
  if (!gServer) return;
  gServer->sendHeader("Cache-Control", "no-store");
  gServer->send_P(200, "text/html", WEBUI_HTML_PROGMEM);
}

void OtaApManager::handleStatus() {
  if (!gServer) return;
  String j = "{\"ap\":true,\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  j += ",\"windowMs\":" + String(OTA_AP_WINDOW_MS);
  j += ",\"elapsedMs\":" + String(millis() - apStartedAtMs_);
  j += ",\"otaReady\":" + String(otaPassword_.isEmpty() ? "false" : "true");
  j += "}";
  gServer->send(200, "application/json", j);
}

void OtaApManager::handleConfig() {
  if (!gServer) return;
  // SECURITY: the open AP never accepts a new OTA password. The OTA password
  // is set only by serial provisioning (existing main.cpp flow).
  gServer->send(403, "text/plain",
                "OTA password must be set over the serial provisioning console");
}

void OtaApManager::handleUpload() {
  if (!gServer) return;
  HTTPUpload& upload = gServer->upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA: WebUI upload start %s\n", upload.filename.c_str());
    // SECURITY: require OTA password as a form field. Reject early if missing.
    const String supplied = gServer->arg("password");
    if (!checkOtaPassword(supplied)) {
      Serial.println("OTA: WebUI upload rejected (bad password)");
      upload.status = UPLOAD_FILE_ABORTED;
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.println("OTA: Update.begin failed");
      upload.status = UPLOAD_FILE_ABORTED;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.println("OTA: Update.write failed");
      upload.status = UPLOAD_FILE_ABORTED;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      Serial.printf("OTA: Update.end failed: %s\n", Update.errorString());
    }
  }
}

void OtaApManager::handleUploadDone() {
  if (!gServer) return;
  const bool ok = !Update.hasError();
  gServer->send(ok ? 200 : 500, "text/plain",
                ok ? "OK: firmware updated, rebooting" : "ERROR: upload failed");
  if (ok) {
    delay(200);
    ESP.restart();
  }
}
