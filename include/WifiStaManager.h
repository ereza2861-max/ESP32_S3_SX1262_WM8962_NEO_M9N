#pragma once
#include <Arduino.h>

class WifiStaManager {
public:
  bool connect(const String& ssid, const String& pass);
  void disconnect();
  bool isConnected() const;
  void task();

private:
  bool connected_ = false;
  String ssid_;
  String pass_;
};
