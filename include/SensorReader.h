// G14/G15/G16: sensor queue policy, BLE identity, and per-node passkey APIs.
#pragma once
#include <Arduino.h>
#include "Config.h"
#include "SensorRegistry.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>

// BLE sensor transport terminates here; radio/MQTT forwarding is queued so BLE
// callbacks never perform network or radio I/O.
class SensorReader {
public:
  enum class SampleQueuePolicy : uint8_t {
    DROP_NEWEST = 0,
    DROP_OLDEST = 1,
  };
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
  uint32_t droppedSamples() const { return droppedSamples_.load(std::memory_order_relaxed); }
  uint8_t queueDepth() const;
  SampleQueuePolicy queuePolicy() const { return queuePolicy_; }
  bool setQueuePolicy(SampleQueuePolicy policy) { queuePolicy_ = policy; return true; }
  bool setPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t passkey);
  bool forgetPeerPasskey(const SensorProtocol::BleAddress& address);
  bool getPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t& passkey) const;
  bool resolvePeerIdentity(const SensorProtocol::BleAddress& advertised,
                           const char* name,
                           SensorProtocol::BleAddress& identity,
                           bool& isRpa) const;
  bool recordPeerRpa(const SensorProtocol::BleAddress& identity,
                     const SensorProtocol::BleAddress& rpa);
  String peersJson() const;

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
    int8_t rssi = -127;
    char nodeName[SensorProtocol::MAX_NODE_NAME_BYTES] = {};
    char sensorName[SensorProtocol::MAX_NAME_BYTES] = {};
    char unit[SensorProtocol::MAX_UNIT_BYTES] = {};
  };

private:
  SensorRegistry registry_;
  QueueHandle_t sensorQueue_ = nullptr;
  StaticQueue_t sensorQueueStruct_{};
  uint8_t sensorQueueStorage_[Config::SENSOR_LORA_QUEUE_DEPTH * sizeof(SensorSample)]{};
  bool initialized_ = false;
  std::atomic<uint32_t> droppedSamples_{0};
  SampleQueuePolicy queuePolicy_ = SampleQueuePolicy::DROP_OLDEST;
  volatile bool forgetRequested_[SensorRegistry::MAX_SUPPORTED_NODES]{};
  volatile bool refreshRequested_[SensorRegistry::MAX_SUPPORTED_NODES]{};
};
