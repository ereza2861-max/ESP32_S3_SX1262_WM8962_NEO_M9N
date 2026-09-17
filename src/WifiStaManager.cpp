#include "WifiStaManager.h"
#include "Config.h"
#include <WiFi.h>

bool WifiStaManager::connect(const String& ssid, const String& pass) {
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) return false;
  ssid_ = ssid; pass_ = pass;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid_.c_str(), pass_.c_str());
  connected_ = false;
  retryDelayMs_ = Config::STA_RETRY_MIN_MS;
  nextRetryMs_ = millis();
  return true;
}

void WifiStaManager::disconnect() {
  WiFi.disconnect(false, false);
  connected_ = false;
}

bool WifiStaManager::isConnected() const {
  return connected_ && WiFi.status() == WL_CONNECTED;
}

void WifiStaManager::task() {
  if (ssid_.isEmpty()) return;
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    if (!connected_) retryDelayMs_ = Config::STA_RETRY_MIN_MS;
    connected_ = true;
    return;
  }
  connected_ = false;
  if (millis() - nextRetryMs_ < retryDelayMs_) return;
  WiFi.begin(ssid_.c_str(), pass_.c_str());
  nextRetryMs_ = millis() + retryDelayMs_;
  retryDelayMs_ = min<uint32_t>(Config::STA_RETRY_MAX_MS, retryDelayMs_ * 2U);
}
