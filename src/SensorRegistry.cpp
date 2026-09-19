// G15: identity-address-first BLE node registry.
#include "SensorRegistry.h"

SensorRegistry::SensorRegistry(size_t maxNodes, size_t maxSensorsPerNode)
    : maxNodes_(maxNodes > MAX_SUPPORTED_NODES ? MAX_SUPPORTED_NODES : maxNodes),
      maxSensorsPerNode_(maxSensorsPerNode > MAX_SUPPORTED_SENSORS_PER_NODE
                             ? MAX_SUPPORTED_SENSORS_PER_NODE
                             : maxSensorsPerNode)
#ifdef ARDUINO
      , mutex_(xSemaphoreCreateRecursiveMutex())
#endif
      {}

bool SensorRegistry::lock(TickType_t timeout) const {
#ifdef ARDUINO
  return mutex_ && xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
#else
  (void)timeout;
  mutex_.lock();
  return true;
#endif
}

void SensorRegistry::unlock() const {
#ifdef ARDUINO
  if (mutex_) (void)xSemaphoreGiveRecursive(mutex_);
#else
  mutex_.unlock();
#endif
}

bool SensorRegistry::upsertNode(const SensorProtocol::BleAddress& address,
                                const char* name, int8_t rssi, uint32_t nowMs,
                                size_t& nodeIndex) {
  if (!lock()) return false;
  const int existing = findNode(address);
  if (existing < 0 && address.type == 1) { unlock(); return false; }
  if (existing >= 0) {
    nodeIndex = static_cast<size_t>(existing);
    Node& n = nodes_[nodeIndex];
    n.rssi = rssi;
    n.lastSeenMs = nowMs;
    if (name && name[0]) {
      std::strncpy(n.name, name, sizeof(n.name) - 1);
      n.name[sizeof(n.name) - 1] = '\0';
    }
    unlock();
    return true;
  }
  if (nodeCount_ >= maxNodes_) {
    bool foundHole = false;
    for (size_t i = 0; i < maxNodes_; ++i) {
      if (!nodes_[i].allocated) { nodeIndex = i; foundHole = true; break; }
    }
    if (!foundHole) { unlock(); return false; }
  } else {
    nodeIndex = 0;
    while (nodeIndex < maxNodes_ && nodes_[nodeIndex].allocated) ++nodeIndex;
    if (nodeIndex >= maxNodes_) { unlock(); return false; }
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
  unlock();
  return true;
}

bool SensorRegistry::markConnected(size_t nodeIndex, bool connected, uint32_t nowMs) {
  if (!lock()) return false;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) { unlock(); return false; }
  nodes_[nodeIndex].connected = connected;
  nodes_[nodeIndex].lastSeenMs = nowMs;
  unlock();
  return true;
}

bool SensorRegistry::evictDisconnected(uint32_t nowMs, uint32_t ttlMs, size_t& nodeIndex) {
  if (!lock()) return false;
  for (size_t i = 0; i < maxNodes_; ++i) {
    if (nodes_[i].allocated && !nodes_[i].connected &&
        nowMs - nodes_[i].lastSeenMs >= ttlMs) {
      nodes_[i] = {};
      --nodeCount_;
      nodeIndex = i;
      unlock();
      return true;
    }
  }
  unlock();
  return false;
}

bool SensorRegistry::forgetNode(size_t nodeIndex) {
  if (!lock()) return false;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) { unlock(); return false; }
  nodes_[nodeIndex] = {};
  if (nodeCount_ > 0) --nodeCount_;
  unlock();
  return true;
}

bool SensorRegistry::clearDescriptors(size_t nodeIndex) {
  if (!lock()) return false;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) { unlock(); return false; }
  nodes_[nodeIndex].sensorCount = 0;
  for (size_t i = 0; i < MAX_SUPPORTED_SENSORS_PER_NODE; ++i) {
    nodes_[nodeIndex].descriptors[i] = {};
    nodes_[nodeIndex].values[i] = {};
    nodes_[nodeIndex].valueValid[i] = false;
  }
  unlock();
  return true;
}

bool SensorRegistry::upsertDescriptor(
    size_t nodeIndex, const SensorProtocol::SensorDescriptor& descriptor) {
  if (!lock()) return false;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated ||
      !SensorProtocol::validDescriptor(descriptor)) { unlock(); return false; }
  Node& n = nodes_[nodeIndex];
  const int existing = findSensor(nodeIndex, descriptor.id);
  if (existing >= 0) { n.descriptors[existing] = descriptor; unlock(); return true; }
  if (n.sensorCount >= maxSensorsPerNode_) { unlock(); return false; }
  n.descriptors[n.sensorCount] = descriptor;
  n.values[n.sensorCount] = {};
  n.valueValid[n.sensorCount] = false;
  ++n.sensorCount;
  unlock();
  return true;
}

bool SensorRegistry::updateValue(size_t nodeIndex,
                                 const SensorProtocol::SensorValue& value,
                                 uint32_t nowMs) {
  if (!lock()) return false;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated ||
      !SensorProtocol::validValue(value)) { unlock(); return false; }
  const int sensor = findSensor(nodeIndex, value.id);
  if (sensor < 0) { unlock(); return false; }
  Node& n = nodes_[nodeIndex];
  n.values[sensor] = value;
  n.valueValid[sensor] = true;
  n.lastSeenMs = nowMs;
  unlock();
  return true;
}

size_t SensorRegistry::sensorCount(size_t nodeIndex) const {
  if (!lock()) return 0;
  const size_t count = nodeIndex < MAX_SUPPORTED_NODES && nodes_[nodeIndex].allocated
                         ? nodes_[nodeIndex].sensorCount : 0;
  unlock();
  return count;
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
  if (!lock()) return -1;
  for (size_t i = 0; i < MAX_SUPPORTED_NODES; ++i) {
    if (nodes_[i].allocated && nodes_[i].address == address) { unlock(); return static_cast<int>(i); }
  }
  for (size_t i = 0; i < MAX_SUPPORTED_NODES; ++i) {
    if (nodes_[i].allocated && nodes_[i].hasRPA && nodes_[i].lastRPA == address) {
      unlock(); return static_cast<int>(i);
    }
  }
  unlock();
  return -1;
}

int SensorRegistry::findNodeByRpa(const SensorProtocol::BleAddress& address) const {
  if (!lock()) return -1;
  for (size_t i = 0; i < MAX_SUPPORTED_NODES; ++i) {
    if (nodes_[i].allocated && nodes_[i].hasRPA && nodes_[i].lastRPA == address) {
      unlock(); return static_cast<int>(i);
    }
  }
  unlock();
  return -1;
}

bool SensorRegistry::setLastRpa(size_t nodeIndex, const SensorProtocol::BleAddress& rpa) {
  if (!lock()) return false;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) {
    unlock(); return false;
  }
  nodes_[nodeIndex].lastRPA = rpa;
  nodes_[nodeIndex].hasRPA = true;
  unlock();
  return true;
}

int SensorRegistry::findSensor(size_t nodeIndex, uint16_t sensorId) const {
  if (!lock()) return -1;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) { unlock(); return -1; }
  for (size_t i = 0; i < nodes_[nodeIndex].sensorCount; ++i) {
    if (nodes_[nodeIndex].descriptors[i].id == sensorId) { unlock(); return static_cast<int>(i); }
  }
  unlock();
  return -1;
}

bool SensorRegistry::snapshot(Snapshot* out, size_t capacity, size_t& count) const {
  count = 0;
  if (!out || capacity == 0 || !lock()) return false;
  const size_t limit = maxNodes_ < capacity ? maxNodes_ : capacity;
  for (size_t i = 0; i < limit; ++i) {
    if (!nodes_[i].allocated) continue;
    out[count].index = i;
    out[count].node = nodes_[i];
    ++count;
  }
  unlock();
  return true;
}

bool SensorRegistry::snapshotNode(size_t nodeIndex, Node& out) const {
  if (!lock()) return false;
  if (nodeIndex >= MAX_SUPPORTED_NODES || !nodes_[nodeIndex].allocated) { unlock(); return false; }
  out = nodes_[nodeIndex];
  unlock();
  return true;
}
