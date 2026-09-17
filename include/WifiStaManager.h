#pragma once
#include <Arduino.h>
#include "Config.h"

class WifiStaManager {
public:
  bool connect(const String& ssid, const String& pass);
  void disconnect();
  bool isConnected() const;
  void task();

private:
  bool connected_ = false;
  uint32_t nextRetryMs_ = 0;
  uint32_t retryDelayMs_ = Config::STA_RETRY_MIN_MS;
  String ssid_;
  String pass_;
};
