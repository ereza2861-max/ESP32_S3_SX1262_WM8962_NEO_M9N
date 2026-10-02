// G14/G15/G16: sensor queue policy, BLE identity, and per-node passkey APIs.
#pragma once
#include <Arduino.h>
#include "Config.h"
#include "SensorRegistry.h"
#include "SensorProtocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>

// Keep the runtime registry and compile-time gateway capacity on one value.
static_assert(Config::SENSOR_MAX_SENSORS_PER_NODE_VALUE ==
                  SensorRegistry::MAX_SUPPORTED_SENSORS_PER_NODE,
              "gateway sensor capacity constants must remain synchronized");

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
  bool enqueueSensorForLoRa(const struct SensorSample& sample, uint32_t sourceSequence = 0, uint8_t schemaVersion = 0, uint32_t firmwareVersion = SensorProtocol::FIRMWARE_VERSION);
  bool popSensorForLoRa(struct SensorSample& sample, TickType_t timeout = 0, uint32_t* sourceSequence = nullptr, uint8_t* schemaVersion = nullptr, uint32_t* firmwareVersion = nullptr);
  bool peekSensorForLoRa(struct SensorSample& sample, uint32_t* sourceSequence = nullptr, uint8_t* schemaVersion = nullptr, uint32_t* firmwareVersion = nullptr) const;
  uint32_t droppedSamples() const { return droppedSamples_.load(std::memory_order_relaxed); }
  uint8_t queueDepth() const;
  uint32_t peerMacFailures() const;
  SampleQueuePolicy queuePolicy() const {
    return static_cast<SampleQueuePolicy>(queuePolicy_.load(std::memory_order_acquire));
  }
  bool setQueuePolicy(SampleQueuePolicy policy) {
    queuePolicy_.store(static_cast<uint8_t>(policy), std::memory_order_release);
    return true;
  }
  bool setPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t passkey);
  bool setPeerIrk(const SensorProtocol::BleAddress& address, const uint8_t irk[16]);
  bool forgetPeerPasskey(const SensorProtocol::BleAddress& address);
  bool getPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t& passkey) const;
  bool resolvePeerIdentity(const SensorProtocol::BleAddress& advertised,
                           const char* name,
                           SensorProtocol::BleAddress& identity,
                           bool& isRpa) const;
  bool recordPeerRpa(const SensorProtocol::BleAddress& identity,
                     const SensorProtocol::BleAddress& rpa);
  String peersJson() const;
  bool sendSensorCommand(size_t nodeIndex, uint8_t commandId,
                         uint16_t sensorId = 0, uint32_t argument = 0);
  bool getSensorCommandResponse(size_t nodeIndex,
                                SensorProtocol::CommandResponse& out) const;

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
  struct QueuedSample {
    SensorSample sample{};
    uint32_t sourceSequence = 0;
    uint8_t schemaVersion = 0;
    uint32_t firmwareVersion = SensorProtocol::FIRMWARE_VERSION;
  };
  uint8_t sensorQueueStorage_[Config::SENSOR_LORA_QUEUE_DEPTH * sizeof(QueuedSample)]{};
  bool initialized_ = false;
  std::atomic<uint32_t> droppedSamples_{0};
  std::atomic<uint8_t> queuePolicy_{static_cast<uint8_t>(SampleQueuePolicy::DROP_OLDEST)};
  std::atomic<bool> forgetRequested_[SensorRegistry::MAX_SUPPORTED_NODES]{};
  std::atomic<bool> refreshRequested_[SensorRegistry::MAX_SUPPORTED_NODES]{};
};
