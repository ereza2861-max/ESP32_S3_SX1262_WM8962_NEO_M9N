#include "SensorReader.h"
#include "Config.h"
#include "MqttClientManager.h"
#include "SensorTelemetry.h"
#include <freertos/queue.h>

#if SENSOR_READER_ENABLED
#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include <NimBLERemoteService.h>
#include <NimBLERemoteCharacteristic.h>
#include <esp_task_wdt.h>
#include <ctime>
#include <cstring>
#include <cmath>
#include <string>
#include <mbedtls/md.h>

namespace {
constexpr uint64_t MIN_VALID_EPOCH_MS = 1700000000000ULL;

struct ClientSlot {
  NimBLEClient* client = nullptr;
  NimBLERemoteCharacteristic* request = nullptr;
  NimBLERemoteCharacteristic* descriptor = nullptr;
  NimBLERemoteCharacteristic* value = nullptr;
  size_t nodeIndex = 0;
  bool inUse = false;
  bool descriptorRefreshRequested = false;
};

SensorReader* gReader = nullptr;
extern MqttClientManager mqtt;

struct PairingFailure {
  SensorProtocol::BleAddress address{};
  uint8_t failures = 0;
  uint32_t blockUntilMs = 0;
};
PairingFailure gPairingFailures[Config::BLE_MAX_BONDS]{};
uint32_t gBlePasskey = 0;

uint32_t nodeIdFromAddress(const SensorProtocol::BleAddress& address) {
  uint32_t hash = 2166136261UL;
  for (uint8_t b : address.bytes) { hash ^= b; hash *= 16777619UL; }
  hash ^= address.type;
  hash *= 16777619UL;
  return hash ? hash : 1U;
}

bool pairingBlocked(const SensorProtocol::BleAddress& address) {
  const uint32_t now = millis();
  for (auto& entry : gPairingFailures) {
    if (entry.failures && entry.address == address &&
        static_cast<int32_t>(now - entry.blockUntilMs) < 0) return true;
  }
  return false;
}

void pairingFailure(const SensorProtocol::BleAddress& address) {
  PairingFailure* selected = nullptr;
  for (auto& entry : gPairingFailures) {
    if (entry.address == address) { selected = &entry; break; }
    if (!selected && entry.failures == 0) selected = &entry;
  }
  if (!selected) selected = &gPairingFailures[0];
  selected->address = address;
  if (selected->failures < 0xFF) ++selected->failures;
  if (selected->failures >= Config::BLE_PAIRING_MAX_FAILURES_VALUE)
    selected->blockUntilMs = millis() + Config::BLE_PAIRING_BLOCK_MS_VALUE;
}

void pairingSuccess(const SensorProtocol::BleAddress& address) {
  for (auto& entry : gPairingFailures) {
    if (entry.address == address) { entry = {}; return; }
  }
}

bool deriveBlePasskey(uint8_t out6[6]) {
  if (!out6 || gConfig.loraKeyHex.length() != 32) return false;
  uint8_t key[16] = {};
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < sizeof(key); ++i) {
    const int h = hex(gConfig.loraKeyHex[i * 2]);
    const int l = hex(gConfig.loraKeyHex[i * 2 + 1]);
    if (h < 0 || l < 0) return false;
    key[i] = static_cast<uint8_t>((h << 4) | l);
  }
  const String message = String(Config::BLE_PAIRING_PASSKEY_DERIVATION_LABEL) + Config::DEVICE_ID;
  uint8_t digest[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
      reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
      digest, sizeof(digest)) != 0) return false;
  uint32_t n = 0;
  std::memcpy(&n, digest, sizeof(n));
  n = 100000U + (n % 900000U);
  for (uint8_t i = 0; i < 6; ++i) {
    out6[5U - i] = static_cast<uint8_t>('0' + (n % 10U));
    n /= 10U;
  }
  return true;
}

uint32_t passkeyValue(const uint8_t digits[6]) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 6; ++i) value = value * 10U + (digits[i] - '0');
  return value;
}

class SensorClientCallbacks final : public NimBLEClientCallbacks {
public:
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    NimBLEDevice::injectPassKey(connInfo, gBlePasskey);
  }
};

SensorClientCallbacks gClientCallbacks;
ClientSlot gSlots[SensorRegistry::MAX_SUPPORTED_NODES]{};
NimBLEScan* gScan = nullptr;

SensorProtocol::BleAddress toBleAddress(const NimBLEAddress& address) {
  SensorProtocol::BleAddress out{};
  const uint8_t* value = address.getVal();
  if (value) std::memcpy(out.bytes, value, sizeof(out.bytes));
  out.type = address.getType();
  return out;
}

uint64_t gatewayTimestampMs() {
  const time_t now = time(nullptr);
  if (now > 1700000000) return static_cast<uint64_t>(now) * 1000ULL;
  return static_cast<uint64_t>(millis());
}

void notifyCallback(NimBLERemoteCharacteristic* characteristic,
                    uint8_t* data, size_t length, bool) {
  if (!gReader || !characteristic || !data || length != sizeof(SensorProtocol::SensorValue)) return;

  for (size_t i = 0; i < Config::SENSOR_MAX_NODES_VALUE; ++i) {
    ClientSlot& slot = gSlots[i];
    if (!slot.inUse || slot.value != characteristic) continue;

    SensorProtocol::SensorValue value{};
    std::memcpy(&value, data, sizeof(value));
    if (!SensorProtocol::validValue(value)) return;

    const int sensorIndex = gReader->registry().findSensor(slot.nodeIndex, value.id);
    if (sensorIndex < 0) {
      // A newly-added sensor is allowed to appear without a reconnect. Ask the
      // BLE task to tear down and re-discover the descriptor list outside the
      // notification callback.
      slot.descriptorRefreshRequested = true;
      return;
    }
    const SensorProtocol::SensorDescriptor* descriptor =
        gReader->registry().descriptor(
        slot.nodeIndex, static_cast<size_t>(sensorIndex));
    if (!descriptor) return;

    if (value.value < descriptor->min || value.value > descriptor->max)
      value.quality |= SensorProtocol::QUALITY_OUT_OF_RANGE;
    if (value.timestamp == 0) {
      value.timestamp = gatewayTimestampMs();
      value.quality |= SensorProtocol::QUALITY_TIMESTAMP_GATEWAY;
    } else if (value.timestamp < MIN_VALID_EPOCH_MS) {
      value.timestamp = gatewayTimestampMs();
      value.quality |= SensorProtocol::QUALITY_TIMESTAMP_INVALID |
                       SensorProtocol::QUALITY_TIMESTAMP_GATEWAY;
    }
    const uint32_t nowMs = millis();
    if (!gReader->registry().updateValue(slot.nodeIndex, value, nowMs)) return;
    const SensorRegistry::Node* node = gReader->registry().node(slot.nodeIndex);
    if (!node) return;
    const SensorProtocol::SensorDescriptor* liveDescriptor =
        gReader->registry().descriptor(slot.nodeIndex, static_cast<size_t>(sensorIndex));
    if (!liveDescriptor) return;

    SensorReader::SensorSample sample{};
    sample.nodeId = nodeIdFromAddress(node->address);
    sample.sensorId = value.id;
    sample.value = value.value;
    sample.quality = value.quality;
    sample.timestampMs = value.timestamp;
    (void)gReader->enqueueSensorForLoRa(sample);
    (void)mqtt.publishSensorData(sample.nodeId, node->name, sample.sensorId,
                                 liveDescriptor->name, liveDescriptor->unit,
                                 sample.value, sample.quality, node->rssi,
                                 sample.timestampMs);
    return;
  }
}

void cleanupSlot(ClientSlot& slot, bool disconnected) {
  if (gReader->registry().node(slot.nodeIndex) != nullptr) {
    gReader->registry().markConnected(slot.nodeIndex, false, millis());
  }
  NimBLEClient* client = slot.client;
  slot.inUse = false;
  slot.descriptorRefreshRequested = false;
  slot.request = nullptr;
  slot.descriptor = nullptr;
  slot.value = nullptr;
  slot.client = nullptr;
  if (client) (void)NimBLEDevice::deleteClient(client);
  slot.nodeIndex = 0;
  (void)disconnected;
}

bool writeDescriptorRequest(ClientSlot& slot, const uint8_t request[3]) {
  if (!slot.request) return false;
  if (slot.request->canWrite()) return slot.request->writeValue(request, 3, true);
  if (slot.request->canWriteNoResponse()) return slot.request->writeValue(request, 3, false);
  return false;
}

bool connectDevice(const NimBLEAdvertisedDevice* device) {
  if (!gReader || !device) return false;
  const SensorProtocol::BleAddress address = toBleAddress(device->getAddress());
  if (gConfig.blePairingEnabled && pairingBlocked(address)) return false;
  size_t nodeIndex = 0;
  const int existingNode = gReader->registry().findNode(address);
  if (existingNode >= 0) {
    nodeIndex = static_cast<size_t>(existingNode);
    (void)gReader->registry().upsertNode(
        address, device->haveName() ? device->getName().c_str() : nullptr,
        device->getRSSI(), millis(), nodeIndex);
  } else {
    if (gReader->registry().isFull() &&
        !gReader->registry().evictDisconnected(millis(), Config::SENSOR_NODE_EVICTION_MS_VALUE, nodeIndex)) {
      return false;
    }
    if (!gReader->registry().upsertNode(
            address, device->haveName() ? device->getName().c_str() : nullptr,
            device->getRSSI(), millis(), nodeIndex)) {
      return false;
    }
  }

  ClientSlot* slot = nullptr;
  for (size_t i = 0; i < Config::SENSOR_MAX_NODES_VALUE; ++i) {
    if (!gSlots[i].inUse) {
      slot = &gSlots[i];
      break;
    }
  }
  if (!slot) return false;

  slot->client = NimBLEDevice::createClient();
  if (!slot->client) {
    gReader->registry().markConnected(nodeIndex, false, millis());
    return false;
  }
  slot->client->setConnectTimeout(Config::SENSOR_CONNECT_TIMEOUT_MS_VALUE);
  slot->client->setClientCallbacks(&gClientCallbacks, false);

  esp_task_wdt_reset();
  if (!slot->client->connect(device)) {
    (void)NimBLEDevice::deleteClient(slot->client);
    slot->client = nullptr;
    gReader->registry().markConnected(nodeIndex, false, millis());
    return false;
  }

  esp_task_wdt_reset();
  if (gConfig.blePairingEnabled) {
    if (!slot->client->secureConnection()) {
      pairingFailure(address);
      (void)NimBLEDevice::deleteClient(slot->client);
      *slot = {};
      gReader->registry().markConnected(nodeIndex, false, millis());
      return false;
    }
    pairingSuccess(address);
  } else if (Config::SENSOR_REQUIRE_ENCRYPTION_VALUE &&
             !slot->client->secureConnection()) {
    (void)NimBLEDevice::deleteClient(slot->client);
    *slot = {};
    gReader->registry().markConnected(nodeIndex, false, millis());
    return false;
  }

  NimBLERemoteService* service = slot->client->getService(
      NimBLEUUID(SensorProtocol::SERVICE_UUID));
  if (!service) {
    (void)NimBLEDevice::deleteClient(slot->client);
    *slot = {};
    gReader->registry().markConnected(nodeIndex, false, millis());
    return false;
  }

  slot->request = service->getCharacteristic(NimBLEUUID(SensorProtocol::DESCRIPTOR_REQUEST_UUID));
  slot->descriptor = service->getCharacteristic(NimBLEUUID(SensorProtocol::DESCRIPTOR_DATA_UUID));
  slot->value = service->getCharacteristic(NimBLEUUID(SensorProtocol::SENSOR_VALUE_UUID));
  if (!slot->request || !slot->descriptor || !slot->value ||
      (!slot->request->canWrite() && !slot->request->canWriteNoResponse()) ||
      !slot->descriptor->canRead() ||
      (!slot->value->canNotify() && !slot->value->canIndicate())) {
    (void)NimBLEDevice::deleteClient(slot->client);
    *slot = {};
    gReader->registry().markConnected(nodeIndex, false, millis());
    return false;
  }

  slot->nodeIndex = nodeIndex;
  slot->inUse = true;
  gReader->registry().markConnected(nodeIndex, true, millis());
  gReader->registry().clearDescriptors(nodeIndex);

  uint8_t request[3] = {SensorProtocol::PROTOCOL_VERSION,
                        SensorProtocol::DESCRIPTOR_REQUEST_LIST, 0};
  if (!writeDescriptorRequest(*slot, request)) {
    cleanupSlot(*slot, true);
    return false;
  }
  const NimBLEAttValue response = slot->descriptor->readValue();
  if (response.size() != sizeof(SensorProtocol::SensorDescriptorResponse)) {
    cleanupSlot(*slot, true);
    return false;
  }

  SensorProtocol::SensorDescriptorResponse first{};
  std::memcpy(&first, response.data(), sizeof(first));
  if (first.magic != SensorProtocol::DESCRIPTOR_MAGIC ||
      first.version != SensorProtocol::PROTOCOL_VERSION || first.index != 0 ||
      first.total == 0 || first.total > Config::SENSOR_MAX_SENSORS_PER_NODE_VALUE ||
      !SensorProtocol::validDescriptor(first.descriptor) ||
      !gReader->registry().upsertDescriptor(nodeIndex, first.descriptor)) {
    cleanupSlot(*slot, true);
    return false;
  }

  for (uint8_t index = 1; index < first.total; ++index) {
    esp_task_wdt_reset();
    request[2] = index;
    if (!writeDescriptorRequest(*slot, request)) {
      cleanupSlot(*slot, true);
      return false;
    }
    const NimBLEAttValue item = slot->descriptor->readValue();
    if (item.size() != sizeof(SensorProtocol::SensorDescriptorResponse)) {
      cleanupSlot(*slot, true);
      return false;
    }
    SensorProtocol::SensorDescriptorResponse decoded{};
    std::memcpy(&decoded, item.data(), sizeof(decoded));
    if (decoded.magic != SensorProtocol::DESCRIPTOR_MAGIC ||
        decoded.version != SensorProtocol::PROTOCOL_VERSION ||
        decoded.index != index || decoded.total != first.total ||
        !SensorProtocol::validDescriptor(decoded.descriptor) ||
        !gReader->registry().upsertDescriptor(nodeIndex, decoded.descriptor)) {
      cleanupSlot(*slot, true);
      return false;
    }
  }

  const bool subscribed = slot->value->canNotify()
                              ? slot->value->subscribe(true, notifyCallback)
                              : slot->value->subscribe(false, notifyCallback);
  if (!subscribed) {
    cleanupSlot(*slot, true);
    return false;
  }
  Serial.printf("SENSOR: connected %s sensors=%u rssi=%d\n",
                device->getAddress().toString().c_str(), first.total, device->getRSSI());
  return true;
}

} // namespace

bool SensorReader::begin(const String& gatewayName) {
  if (!Config::SENSOR_READER_ENABLED_VALUE) return false;
  if (initialized_) return true;
  if (!NimBLEDevice::isInitialized() &&
      !NimBLEDevice::init(std::string(gatewayName.c_str()))) return false;
  if (gConfig.blePairingEnabled) {
    NimBLEDevice::setSecurityAuth(true /* bonding */, true /* MITM */, true /* SC */);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
    uint8_t passkey[6] = {};
    if (!deriveBlePasskey(passkey)) return false;
    gBlePasskey = passkeyValue(passkey);
    NimBLEDevice::setSecurityPasskey(gBlePasskey);
  }
  (void)NimBLEDevice::setMTU(Config::SENSOR_MTU_VALUE);
  gScan = NimBLEDevice::getScan();
  if (!gScan) return false;
  gScan->setActiveScan(Config::SENSOR_ACTIVE_SCAN_VALUE);
  gScan->setInterval(Config::SENSOR_SCAN_INTERVAL_MS_VALUE);
  gScan->setWindow(Config::SENSOR_SCAN_WINDOW_MS_VALUE);
  gScan->setMaxResults(static_cast<uint8_t>(Config::SENSOR_MAX_NODES_VALUE));
  sensorQueue_ = xQueueCreateStatic(Config::SENSOR_LORA_QUEUE_DEPTH, sizeof(SensorSample),
                                    sensorQueueStorage_, &sensorQueueStruct_);
  if (!sensorQueue_) return false;
  gReader = this;
  initialized_ = true;
  Serial.printf("SENSOR: reader enabled, NimBLE=%s maxNodes=%u maxSensors=%u\n",
                NimBLEDevice::getVersion(),
                static_cast<unsigned>(Config::SENSOR_MAX_NODES_VALUE),
                static_cast<unsigned>(Config::SENSOR_MAX_SENSORS_PER_NODE_VALUE));
  return true;
}

void SensorReader::task() {
  if (!initialized_ || !Config::SENSOR_READER_ENABLED_VALUE || !gScan) return;

  // WebUI actions are consumed by the BLE task; the HTTP handler never tears
  // down a NimBLE client or mutates the registry directly.
  for (size_t nodeIndex = 0; nodeIndex < Config::SENSOR_MAX_NODES_VALUE; ++nodeIndex) {
    if (forgetRequested_[nodeIndex]) {
      for (size_t i = 0; i < Config::SENSOR_MAX_NODES_VALUE; ++i) {
        if (gSlots[i].inUse && gSlots[i].nodeIndex == nodeIndex) cleanupSlot(gSlots[i], true);
      }
      (void)registry_.forgetNode(nodeIndex);
      forgetRequested_[nodeIndex] = false;
      refreshRequested_[nodeIndex] = false;
    } else if (refreshRequested_[nodeIndex]) {
      for (size_t i = 0; i < Config::SENSOR_MAX_NODES_VALUE; ++i) {
        if (gSlots[i].inUse && gSlots[i].nodeIndex == nodeIndex) cleanupSlot(gSlots[i], true);
      }
      refreshRequested_[nodeIndex] = false;
    }
  }

  for (size_t i = 0; i < Config::SENSOR_MAX_NODES_VALUE; ++i) {
    ClientSlot& slot = gSlots[i];
    if (!slot.inUse) continue;
    if (!slot.client || !slot.client->isConnected() || slot.descriptorRefreshRequested)
      cleanupSlot(slot, true);
  }

  size_t active = 0;
  for (size_t i = 0; i < Config::SENSOR_MAX_NODES_VALUE; ++i) active += gSlots[i].inUse ? 1U : 0U;
  if (active < Config::SENSOR_MAX_NODES_VALUE) {
    esp_task_wdt_reset();
    const NimBLEScanResults results = gScan->getResults(Config::SENSOR_SCAN_DURATION_MS_VALUE, false);
    for (int i = 0; i < results.getCount() && active < Config::SENSOR_MAX_NODES_VALUE; ++i) {
      esp_task_wdt_reset();
      const NimBLEAdvertisedDevice* device = results.getDevice(static_cast<uint32_t>(i));
      if (!device || !device->isAdvertisingService(NimBLEUUID(SensorProtocol::SERVICE_UUID))) continue;
      const SensorProtocol::BleAddress address = toBleAddress(device->getAddress());
      const int existingNode = registry_.findNode(address);
      if (existingNode >= 0) {
        const SensorRegistry::Node* node = registry_.node(static_cast<size_t>(existingNode));
        if (node && node->connected) continue;
      }
      if (connectDevice(device)) ++active;
    }
    gScan->clearResults();
  }

  vTaskDelay(pdMS_TO_TICKS(Config::SENSOR_TASK_PERIOD_MS_VALUE));
}

bool SensorReader::enqueueSensorForLoRa(const SensorSample& sample) {
  return sensorQueue_ && xQueueSend(sensorQueue_, &sample, 0) == pdTRUE;
}

bool SensorReader::popSensorForLoRa(SensorSample& sample, TickType_t timeout) {
  return sensorQueue_ && xQueueReceive(sensorQueue_, &sample, timeout) == pdTRUE;
}

bool SensorReader::snapshotNodes(SensorNodeSnapshot* out, size_t capacity, size_t& count) const {
  return registry_.snapshot(out, capacity, count);
}

bool SensorReader::snapshotNode(size_t nodeIndex, SensorRegistry::Node& out) const {
  return registry_.snapshotNode(nodeIndex, out);
}

bool SensorReader::requestForgetNode(size_t nodeIndex) {
  SensorRegistry::Node node{};
  if (nodeIndex >= SensorRegistry::MAX_SUPPORTED_NODES || !registry_.snapshotNode(nodeIndex, node)) return false;
  forgetRequested_[nodeIndex] = true;
  return true;
}

bool SensorReader::requestRefreshNode(size_t nodeIndex) {
  SensorRegistry::Node node{};
  if (nodeIndex >= SensorRegistry::MAX_SUPPORTED_NODES || !registry_.snapshotNode(nodeIndex, node)) return false;
  refreshRequested_[nodeIndex] = true;
  return true;
}

bool SensorReader::isEnabled() const { return Config::SENSOR_READER_ENABLED_VALUE; }

bool SensorReader::hasConnectedNode() const {
  for (size_t i = 0; i < Config::SENSOR_MAX_NODES_VALUE; ++i) {
    const SensorRegistry::Node* node = registry_.node(i);
    if (node && node->connected) return true;
  }
  return false;
}

#else

bool SensorReader::begin(const String&) { return false; }
bool SensorReader::enqueueSensorForLoRa(const SensorSample&) { return false; }
bool SensorReader::popSensorForLoRa(SensorSample&, TickType_t) { return false; }
bool SensorReader::snapshotNodes(SensorNodeSnapshot*, size_t, size_t& count) const { count = 0; return false; }
bool SensorReader::snapshotNode(size_t, SensorRegistry::Node&) const { return false; }
bool SensorReader::requestForgetNode(size_t) { return false; }
bool SensorReader::requestRefreshNode(size_t) { return false; }
void SensorReader::task() {}
bool SensorReader::isEnabled() const { return false; }
bool SensorReader::hasConnectedNode() const { return false; }

#endif
