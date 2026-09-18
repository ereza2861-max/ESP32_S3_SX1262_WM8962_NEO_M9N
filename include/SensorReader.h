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
  using SensorNodeSnapshot = SensorRegistry::Snapshot;
  bool begin(const String& gatewayName);
  void task();
  bool isEnabled() const;
  bool hasConnectedNode() const;
  size_t nodeCount() const { return registry_.nodeCount(); }
  SensorRegistry& registry() { return registry_; }
  const SensorRegistry& registry() const { return registry_; }
  bool enqueueSensorForLoRa(const struct SensorSample& sample);
  bool popSensorForLoRa(struct SensorSample& sample, TickType_t timeout = 0);

  // UI-facing copy-out APIs. They never expose registry pointers and hold the
  // registry mutex only for the memcpy-sized snapshot operation.
  bool snapshotNodes(SensorNodeSnapshot* out, size_t capacity, size_t& count) const;
  bool snapshotNode(size_t nodeIndex, SensorRegistry::Node& out) const;
  bool requestForgetNode(size_t nodeIndex);
  bool requestRefreshNode(size_t nodeIndex);

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
  volatile bool forgetRequested_[SensorRegistry::MAX_SUPPORTED_NODES]{};
  volatile bool refreshRequested_[SensorRegistry::MAX_SUPPORTED_NODES]{};
};
