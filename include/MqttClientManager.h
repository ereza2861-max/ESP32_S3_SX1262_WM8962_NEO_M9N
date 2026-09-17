#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

class MqttClientManager {
public:
  bool begin();
  bool connect(const String& host, uint16_t port,
               const String& user, const String& pass);
  bool publish(const String& topic, const String& payload, bool retained = false);
  void task();
  bool isConnected() const { return connected_ && client_.connected(); }

private:
  WiFiClientSecure net_;
  PubSubClient client_{net_};
  bool connected_ = false;
  String host_;
  String user_;
  String pass_;
  uint16_t port_ = 1883;
  uint32_t nextRetryMs_ = 0;
  uint32_t retryDelayMs_ = 5000;
  bool loadCredentials();
  bool timeSynchronized() const;
  String topic(const char* leaf) const;
};
