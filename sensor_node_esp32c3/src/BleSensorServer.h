#pragma once

#include <Arduino.h>
#include "SensorRegistry.h"

class NimBLEServer;
class NimBLEService;
class NimBLECharacteristic;

class BleSensorServer {
public:
  bool begin(const String& nodeName, SensorRegistry& registry);
  void task();
  bool isRunning() const { return running_; }
  void setClientConnected(bool connected) { clientConnected_ = connected; }

private:
  void updateDescriptorResponse(uint8_t index);
  void notifyValues();
  void startAdvertising();

  String nodeName_;
  SensorRegistry* registry_ = nullptr;
  NimBLEServer* server_ = nullptr;
  NimBLEService* service_ = nullptr;
  NimBLECharacteristic* request_ = nullptr;
  NimBLECharacteristic* descriptor_ = nullptr;
  NimBLECharacteristic* value_ = nullptr;
  uint32_t advertisingStartedAtMs_ = 0;
  uint32_t lastNotifyMs_ = 0;
  bool running_ = false;
  bool clientConnected_ = false;
};
