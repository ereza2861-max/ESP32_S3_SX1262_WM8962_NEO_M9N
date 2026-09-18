#pragma once
#include "SensorProtocol.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

class SensorRegistry {
public:
  static constexpr size_t MAX_SUPPORTED_NODES = 8;
  static constexpr size_t MAX_SUPPORTED_SENSORS_PER_NODE = 16;

  struct Node {
    SensorProtocol::BleAddress address{};
    char name[SensorProtocol::MAX_NODE_NAME_BYTES] = {};
    int8_t rssi = -127;
    uint32_t lastSeenMs = 0;
    bool allocated = false;
    bool connected = false;
    size_t sensorCount = 0;
    SensorProtocol::SensorDescriptor descriptors[MAX_SUPPORTED_SENSORS_PER_NODE]{};
    SensorProtocol::SensorValue values[MAX_SUPPORTED_SENSORS_PER_NODE]{};
    bool valueValid[MAX_SUPPORTED_SENSORS_PER_NODE]{};
  };

  explicit SensorRegistry(size_t maxNodes = MAX_SUPPORTED_NODES,
                          size_t maxSensorsPerNode = MAX_SUPPORTED_SENSORS_PER_NODE);

  bool upsertNode(const SensorProtocol::BleAddress& address, const char* name,
                  int8_t rssi, uint32_t nowMs, size_t& nodeIndex);
  bool markConnected(size_t nodeIndex, bool connected, uint32_t nowMs);
  bool clearDescriptors(size_t nodeIndex);
  bool evictDisconnected(uint32_t nowMs, uint32_t ttlMs, size_t& nodeIndex);
  bool upsertDescriptor(size_t nodeIndex, const SensorProtocol::SensorDescriptor& descriptor);
  bool updateValue(size_t nodeIndex, const SensorProtocol::SensorValue& value, uint32_t nowMs);

  size_t nodeCount() const { return nodeCount_; }
  size_t sensorCount(size_t nodeIndex) const;
  const Node* node(size_t nodeIndex) const;
  const SensorProtocol::SensorDescriptor* descriptor(size_t nodeIndex, size_t sensorIndex) const;
  const SensorProtocol::SensorValue* value(size_t nodeIndex, size_t sensorIndex) const;

  int findNode(const SensorProtocol::BleAddress& address) const;
  int findSensor(size_t nodeIndex, uint16_t sensorId) const;
  bool isFull() const { return nodeCount_ >= maxNodes_; }

private:
  size_t maxNodes_;
  size_t maxSensorsPerNode_;
  size_t nodeCount_ = 0;
  Node nodes_[MAX_SUPPORTED_NODES]{};
};
