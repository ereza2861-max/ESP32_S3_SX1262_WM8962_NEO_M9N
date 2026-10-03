#pragma once

#include <Arduino.h>
#include "SensorRegistry.h"
#include "SensorProtocol.h"

class NimBLEServer;
class NimBLEService;
class NimBLECharacteristic;
class NimBLEConnInfo;

class BleSensorServer {
public:
  using CommandHandler = bool (*)(const SensorProtocol::CommandRequest&, SensorProtocol::CommandResponse&);
  bool begin(const String& nodeName, SensorRegistry& registry);
  void task();
  bool isRunning() const { return running_; }
  void setClientConnected(bool connected) { clientConnected_ = connected; }
  void setCommandHandler(CommandHandler handler) { commandHandler_ = handler; }
  void refreshDescriptorCache() { updateDescriptorResponse(0); }
  void handleCommand(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo);
  void notifyRomList(const SensorProtocol::RomListNotification* packets, size_t count);

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
  NimBLECharacteristic* command_ = nullptr;
  NimBLECharacteristic* commandResponse_ = nullptr;
  uint32_t advertisingStartedAtMs_ = 0;
  uint32_t lastNotifyMs_ = 0;
  bool running_ = false;
  bool clientConnected_ = false;
  CommandHandler commandHandler_ = nullptr;
};
