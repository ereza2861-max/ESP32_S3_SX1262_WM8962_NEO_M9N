#include "BleSensorReader.h"
#include "Config.h"

bool BleSensorReader::begin(const String& deviceName) {
  deviceName_ = deviceName;
  ready_ = sensorReader_.begin(deviceName_);
  retryAtMs_ = millis() + Config::SENSOR_TASK_PERIOD_MS_VALUE * 4U;
  return ready_;
}

void BleSensorReader::task() {
  if (!Config::SENSOR_READER_ENABLED_VALUE) return;
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
