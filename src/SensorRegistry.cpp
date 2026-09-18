#include "SensorRegistry.h"

SensorRegistry::SensorRegistry(size_t maxNodes, size_t maxSensorsPerNode)
    : maxNodes_(maxNodes > MAX_SUPPORTED_NODES ? MAX_SUPPORTED_NODES : maxNodes),
      maxSensorsPerNode_(maxSensorsPerNode > MAX_SUPPORTED_SENSORS_PER_NODE
                             ? MAX_SUPPORTED_SENSORS_PER_NODE
                             : maxSensorsPerNode) {}

bool SensorRegistry::upsertNode(const SensorProtocol::BleAddress& address,
                                const char* name, int8_t rssi, uint32_t nowMs,
                                size_t& nodeIndex) {
  const int existing = findNode(address);
  if (existing >= 0) {
    nodeIndex = static_cast<size_t>(existing);
    Node& n = nodes_[nodeIndex];
    n.rssi = rssi;
    n.lastSeenMs = nowMs;
    if (name && name[0]) {
      std::strncpy(n.name, name, sizeof(n.name) - 1);
      n.name[sizeof(n.name) - 1] = '\0';
    }
    return true;
  }
  if (nodeCount_ >= maxNodes_) {
    bool foundHole = false;
    for (size_t i = 0; i < maxNodes_; ++i) {
      if (!nodes_[i].allocated) {
        nodeIndex = i;
        foundHole = true;
        break;
      }
    }
    if (!foundHole) return false;
  } else {
    nodeIndex = 0;
    while (nodeIndex < maxNodes_ && nodes_[nodeIndex].allocated) ++nodeIndex;
    if (nodeIndex >= maxNodes_) return false;
  }
  ++nodeCount_;
  Node& n = nodes_[nodeIndex];
  n = {};
  n.allocated = true;
  n.address = address;
  n.rssi = rssi;
  n.lastSeenMs = nowMs;
  n.connected = false;
  n.sensorCount = 0;
  if (name && name[0]) {
    std::strncpy(n.name, name, sizeof(n.name) - 1);
    n.name[sizeof(n.name) - 1] = '\0';
  }
  return true;
}

bool SensorRegistry::markConnected(size_t nodeIndex, bool connected, uint32_t nowMs) {
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) return false;
  nodes_[nodeIndex].connected = connected;
  nodes_[nodeIndex].lastSeenMs = nowMs;
  return true;
}

bool SensorRegistry::evictDisconnected(uint32_t nowMs, uint32_t ttlMs, size_t& nodeIndex) {
  for (size_t i = 0; i < maxNodes_; ++i) {
    if (nodes_[i].allocated && !nodes_[i].connected &&
        nowMs - nodes_[i].lastSeenMs >= ttlMs) {
      nodes_[i] = {};
      --nodeCount_;
      nodeIndex = i;
      return true;
    }
  }
  return false;
}

bool SensorRegistry::clearDescriptors(size_t nodeIndex) {
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) return false;
  nodes_[nodeIndex].sensorCount = 0;
  for (size_t i = 0; i < MAX_SUPPORTED_SENSORS_PER_NODE; ++i) {
    nodes_[nodeIndex].descriptors[i] = {};
    nodes_[nodeIndex].values[i] = {};
    nodes_[nodeIndex].valueValid[i] = false;
  }
  return true;
}

bool SensorRegistry::upsertDescriptor(
    size_t nodeIndex, const SensorProtocol::SensorDescriptor& descriptor) {
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated ||
      !SensorProtocol::validDescriptor(descriptor)) return false;
  Node& n = nodes_[nodeIndex];
  const int existing = findSensor(nodeIndex, descriptor.id);
  if (existing >= 0) {
    n.descriptors[existing] = descriptor;
    return true;
  }
  if (n.sensorCount >= maxSensorsPerNode_) return false;
  n.descriptors[n.sensorCount] = descriptor;
  n.values[n.sensorCount] = {};
  n.valueValid[n.sensorCount] = false;
  ++n.sensorCount;
  return true;
}

bool SensorRegistry::updateValue(size_t nodeIndex,
                                 const SensorProtocol::SensorValue& value,
                                 uint32_t nowMs) {
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated ||
      !SensorProtocol::validValue(value)) return false;
  const int sensor = findSensor(nodeIndex, value.id);
  if (sensor < 0) return false;
  Node& n = nodes_[nodeIndex];
  n.values[sensor] = value;
  n.valueValid[sensor] = true;
  n.lastSeenMs = nowMs;
  return true;
}

size_t SensorRegistry::sensorCount(size_t nodeIndex) const {
  return nodeIndex < MAX_SUPPORTED_NODES && nodes_[nodeIndex].allocated
             ? nodes_[nodeIndex].sensorCount : 0;
}

const SensorRegistry::Node* SensorRegistry::node(size_t nodeIndex) const {
  return nodeIndex < MAX_SUPPORTED_NODES && nodes_[nodeIndex].allocated
             ? &nodes_[nodeIndex] : nullptr;
}

const SensorProtocol::SensorDescriptor* SensorRegistry::descriptor(
    size_t nodeIndex, size_t sensorIndex) const {
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated ||
      sensorIndex >= nodes_[nodeIndex].sensorCount) return nullptr;
  return &nodes_[nodeIndex].descriptors[sensorIndex];
}

const SensorProtocol::SensorValue* SensorRegistry::value(
    size_t nodeIndex, size_t sensorIndex) const {
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated ||
      sensorIndex >= nodes_[nodeIndex].sensorCount ||
      !nodes_[nodeIndex].valueValid[sensorIndex]) return nullptr;
  return &nodes_[nodeIndex].values[sensorIndex];
}

int SensorRegistry::findNode(const SensorProtocol::BleAddress& address) const {
  for (size_t i = 0; i < MAX_SUPPORTED_NODES; ++i) {
    if (nodes_[i].allocated && nodes_[i].address == address) return static_cast<int>(i);
  }
  return -1;
}

int SensorRegistry::findSensor(size_t nodeIndex, uint16_t sensorId) const {
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) return -1;
  for (size_t i = 0; i < nodes_[nodeIndex].sensorCount; ++i) {
    if (nodes_[nodeIndex].descriptors[i].id == sensorId) return static_cast<int>(i);
  }
  return -1;
}
