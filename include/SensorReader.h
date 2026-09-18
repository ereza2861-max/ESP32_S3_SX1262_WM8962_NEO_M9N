#pragma once
#include <Arduino.h>
#include "Config.h"
#include "SensorRegistry.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// BLE sensor transport terminates here; radio/MQTT forwarding is queued so BLE
// callbacks never perform network or radio I/O.
class SensorReader {
public:
  bool begin(const String& gatewayName);
  void task();
  bool isEnabled() const;
  bool hasConnectedNode() const;
  size_t nodeCount() const { return registry_.nodeCount(); }
  SensorRegistry& registry() { return registry_; }
  const SensorRegistry& registry() const { return registry_; }
  bool enqueueSensorForLoRa(const struct SensorSample& sample);
  bool popSensorForLoRa(struct SensorSample& sample, TickType_t timeout = 0);

public:
  struct SensorSample {
    uint32_t nodeId = 0;
    uint16_t sensorId = 0;
    float value = 0.0f;
    uint8_t quality = 0;
    uint64_t timestampMs = 0;
  };

private:
  SensorRegistry registry_;
  QueueHandle_t sensorQueue_ = nullptr;
  StaticQueue_t sensorQueueStruct_{};
  uint8_t sensorQueueStorage_[Config::SENSOR_LORA_QUEUE_DEPTH * sizeof(SensorSample)]{};
  bool initialized_ = false;
};
