#pragma once
#include <Arduino.h>

class MqttClientManager {
public:
  bool connect(const String& host, uint16_t port,
               const String& user, const String& pass);
  bool publish(const String& topic, const String& payload);
  void task();

private:
  bool connected_ = false;
};
