#include "WifiStaManager.h"
#include <WiFi.h>

bool WifiStaManager::connect(const String& ssid, const String& pass) {
  // TODO: persist credentials in namespace "wifi_sta" and add retry/backoff.
  ssid_ = ssid;
  pass_ = pass;
  WiFi.begin(ssid.c_str(), pass.c_str());
  connected_ = false;
  return true;
}

void WifiStaManager::disconnect() {
  WiFi.disconnect(true, false);
  connected_ = false;
}

bool WifiStaManager::isConnected() const {
  return connected_ && WiFi.status() == WL_CONNECTED;
}

void WifiStaManager::task() {
  // TODO: implement bounded STA retry/backoff and telemetry integration.
  connected_ = WiFi.status() == WL_CONNECTED;
}
