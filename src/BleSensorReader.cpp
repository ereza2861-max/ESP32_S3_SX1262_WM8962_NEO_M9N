// G16: per-node BLE passkey provisioning facade.
#include "BleSensorReader.h"
#include "Config.h"
#include "PersistentConfig.h"

bool BleSensorReader::begin(const String& deviceName) {
  deviceName_ = deviceName;
  ready_ = sensorReader_.begin(deviceName_);
  retryAtMs_ = millis() + Config::SENSOR_TASK_PERIOD_MS_VALUE * 4U;
  return ready_;
}

void BleSensorReader::task() {
  if (!gConfig.sensorReaderEnabled) return;
  const uint32_t now = millis();
  if (!ready_) {
    if (static_cast<int32_t>(now - retryAtMs_) >= 0) {
      ready_ = sensorReader_.begin(deviceName_);
      retryAtMs_ = now + Config::SENSOR_TASK_PERIOD_MS_VALUE * 4U;
    }
    return;
  }
  sensorReader_.task();
}

bool BleSensorReader::isReady() const { return ready_; }

bool BleSensorReader::hasConnectedSensorNode() const {
  return ready_ && sensorReader_.hasConnectedNode();
}

bool BleSensorReader::setPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t passkey) {
  return sensorReader_.setPeerPasskey(address, passkey);
}

bool BleSensorReader::setPeerIrk(const SensorProtocol::BleAddress& address, const uint8_t irk[16]) {
  return sensorReader_.setPeerIrk(address, irk);
}
bool BleSensorReader::forgetPeerPasskey(const SensorProtocol::BleAddress& address) {
  return sensorReader_.forgetPeerPasskey(address);
}
String BleSensorReader::peersJson() const { return sensorReader_.peersJson(); }
