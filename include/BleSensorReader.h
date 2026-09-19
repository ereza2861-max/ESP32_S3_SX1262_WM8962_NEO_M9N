// G16: per-node BLE passkey provisioning facade.
#pragma once
#include <Arduino.h>
#include "SensorReader.h"

// BLE subsystem facade for the NimBLE GATT sensor-reader role. It owns retry
// behavior while SensorReader owns the GATT protocol and registry.
class BleSensorReader {
public:
  bool begin(const String& deviceName);
  void task();
  bool isReady() const;
  bool hasConnectedSensorNode() const;
  SensorReader& sensorReader() { return sensorReader_; }
  const SensorReader& sensorReader() const { return sensorReader_; }
  bool setPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t passkey);
  bool forgetPeerPasskey(const SensorProtocol::BleAddress& address);
  String peersJson() const;

private:
  SensorReader sensorReader_;
  String deviceName_;
  uint32_t retryAtMs_ = 0;
  bool ready_ = false;
};
