// G14/G15/G16: queue saturation, BLE identity resolution, and peer passkey storage.
#include "SensorReader.h"
#include "Config.h"
#include "PersistentConfig.h"
#include "MqttClientManager.h"
#include "SensorTelemetry.h"
#include "BlePeerStore.h"
#include <freertos/queue.h>

static_assert(sizeof(SensorProtocol::SensorDescriptorResponse) == 68,
              "BLE descriptor response contract must remain 68 bytes");
static_assert(sizeof(SensorProtocol::SensorValue) == 20,
              "BLE sensor value contract must remain 20 bytes");

#if SENSOR_READER_ENABLED
#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include <NimBLERemoteService.h>
#include <NimBLERemoteCharacteristic.h>
#include <host/ble_hs.h>
#include <esp_task_wdt.h>
#include <ctime>
#include <cstring>
#include <cmath>
#include <string>
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <Preferences.h>
#include <nvs_flash.h>

namespace {
constexpr uint64_t MIN_VALID_EPOCH_MS = 1700000000000ULL;

struct ClientSlot {
  NimBLEClient* client = nullptr;
  NimBLERemoteCharacteristic* request = nullptr;
  NimBLERemoteCharacteristic* descriptor = nullptr;
  NimBLERemoteCharacteristic* value = nullptr;
  NimBLERemoteCharacteristic* command = nullptr;
  NimBLERemoteCharacteristic* commandResponse = nullptr;
  size_t nodeIndex = 0;
  bool inUse = false;
  bool descriptorRefreshRequested = false;
  uint32_t pendingCommandSequence = 0;
  uint8_t pendingCommandId = 0;
  uint8_t pendingCommandRetries = 0;
  uint32_t pendingCommandSentMs = 0;
  SensorProtocol::CommandRequest pendingCommand{};
  bool commandPending = false;
  SensorProtocol::CommandResponse lastCommandResponse{};
  bool commandResponseReady = false;
  bool commandCompleted = false;
};

SensorReader* gReader = nullptr;
extern ClientSlot gSlots[SensorRegistry::MAX_SUPPORTED_NODES];
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
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  PairingFailure* selected = nullptr;
  for (auto& entry : gPairingFailures) {
    if (entry.address == address) { selected = &entry; break; }
    if (!selected && entry.failures == 0) selected = &entry;
  }
  if (!selected) selected = &gPairingFailures[0];
  selected->address = address;
  if (selected->failures < 0xFF) ++selected->failures;
  if (selected->failures >= config.blePairingFailureThreshold)
    selected->blockUntilMs = millis() + config.blePairingBlockMs;
}

void pairingSuccess(const SensorProtocol::BleAddress& address) {
  for (auto& entry : gPairingFailures) {
    if (entry.address == address) { entry = {}; return; }
  }
}

bool deriveBlePasskey(uint8_t out6[6]) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (!out6 || config.loraKeyHex.length() != 32) return false;
  uint8_t key[16] = {};
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < sizeof(key); ++i) {
    const int h = hex(config.loraKeyHex[i * 2]);
    const int l = hex(config.loraKeyHex[i * 2 + 1]);
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


constexpr size_t PEER_MAX = Config::BLE_MAX_BONDS;
using PeerRecord = BlePeerStore::PeerRecordV2;

SemaphoreHandle_t gPeerMutex = nullptr;
std::atomic<uint32_t> gPeerMacFailures{0};

bool peerMutexLock() {
  if (!gPeerMutex) gPeerMutex = xSemaphoreCreateMutex();
  return gPeerMutex && xSemaphoreTake(gPeerMutex, pdMS_TO_TICKS(200)) == pdTRUE;
}
void peerMutexUnlock() { if (gPeerMutex) xSemaphoreGive(gPeerMutex); }

bool peerFresh(uint32_t updatedEpoch) {
  const time_t now = time(nullptr);
  return updatedEpoch == 0 || now < 1700000000 ||
         now <= static_cast<time_t>(updatedEpoch) ||
         static_cast<uint32_t>(now - static_cast<time_t>(updatedEpoch)) <= 3600U;
}

bool isRpaAddress(const SensorProtocol::BleAddress& address) {
  return address.type == 1U && (address.bytes[5] & 0xC0U) == 0x40U;
}

bool hasIrk(const uint8_t irk[16]) {
  uint8_t zero[16] = {};
  return irk && std::memcmp(irk, zero, sizeof(zero)) != 0;
}

bool peerStoreSave(const PeerRecord in[PEER_MAX]);

bool peerStoreLoad(PeerRecord out[PEER_MAX]) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (!out || !config.loraKeyHex.length()) return false;
  std::memset(out, 0, sizeof(PeerRecord) * PEER_MAX);

  Preferences prefs;
  if (!prefs.begin("ble_peer", true)) return false;
  const size_t bytes = prefs.getBytesLength("peers");
  if (bytes == 0) {
    prefs.end();
    return true;
  }

  if (bytes == BlePeerStore::V1_BYTES * PEER_MAX) {
    uint8_t blob[BlePeerStore::V1_BYTES * PEER_MAX] = {};
    const size_t got = prefs.getBytes("peers", blob, sizeof(blob));
    prefs.end();
    if (got != sizeof(blob)) return false;

    // V1 is decrypted with the legacy AES-ECB store only during migration.
    uint8_t key[16] = {};
    if (config.loraKeyHex.length() != 32) return false;
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i < sizeof(key); ++i) {
      const int hi = hex(config.loraKeyHex[i * 2]);
      const int lo = hex(config.loraKeyHex[i * 2 + 1]);
      if (hi < 0 || lo < 0) return false;
      key[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, key, 128) != 0) {
      mbedtls_aes_free(&aes);
      return false;
    }
    for (size_t off = 0; off < sizeof(blob); off += 16U) {
      uint8_t block[16] = {};
      std::memcpy(block, blob + off, sizeof(block));
      if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, block, block) != 0) {
        mbedtls_aes_free(&aes);
        return false;
      }
      std::memcpy(blob + off, block, sizeof(block));
    }
    mbedtls_aes_free(&aes);

    bool migrated = false;
    for (size_t i = 0; i < PEER_MAX; ++i) {
      BlePeerStore::PeerRecordV1 legacy{};
      std::memcpy(&legacy, blob + i * BlePeerStore::V1_BYTES, sizeof(legacy));
      if (!BlePeerStore::validV1(legacy)) continue;
      PeerRecord record{};
      record.magic = BlePeerStore::MAGIC;
      record.version = BlePeerStore::VERSION;
      BlePeerStore::setPasskey(record, legacy.passkey);
      record.identity = legacy.identity;
      record.lastRpa = legacy.lastRpa;
      std::memcpy(record.name, legacy.name, sizeof(record.name));
      record.updatedEpoch = legacy.updatedEpoch;
      // V1 did not contain an IRK; manual provisioning can add one later.
      out[i] = record;
      migrated = true;
    }
    if (migrated) (void)peerStoreSave(out);
    return true;
  }

  const bool legacyV2 = bytes == BlePeerStore::V2_LEGACY_BYTES * PEER_MAX;
  if (!legacyV2 && bytes != BlePeerStore::V2_BYTES * PEER_MAX) {
    prefs.end();
    return false;
  }

  const size_t recordBytes = legacyV2 ? BlePeerStore::V2_LEGACY_BYTES : BlePeerStore::V2_BYTES;
  uint8_t blob[BlePeerStore::V2_BYTES * PEER_MAX] = {};
  const size_t got = prefs.getBytes("peers", blob, recordBytes * PEER_MAX);
  prefs.end();
  if (got != recordBytes * PEER_MAX) return false;

  bool migrated = false;
  for (size_t i = 0; i < PEER_MAX; ++i) {
    PeerRecord record{};
    if (legacyV2) {
      BlePeerStore::PeerRecordV2Legacy legacy{};
      std::memcpy(&legacy, blob + i * recordBytes, sizeof(legacy));
      if (legacy.magic != BlePeerStore::MAGIC ||
          !BlePeerStore::openV2Legacy(legacy, config.loraKeyHex.c_str())) {
        if (legacy.magic == BlePeerStore::MAGIC)
          gPeerMacFailures.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      record.magic = BlePeerStore::MAGIC;
      record.version = BlePeerStore::VERSION;
      std::memcpy(record.passkey, legacy.passkey, sizeof(record.passkey));
      record.identity = legacy.identity;
      record.lastRpa = legacy.lastRpa;
      std::memcpy(record.name, legacy.name, sizeof(record.name));
      record.updatedEpoch = legacy.updatedEpoch;
      std::memcpy(record.irk, legacy.irk, sizeof(record.irk));
      // Legacy v2 is decrypted above; re-seal below with the expanded layout.
      migrated = true;
    } else {
      std::memcpy(&record, blob + i * recordBytes, sizeof(record));
      if (record.magic != BlePeerStore::MAGIC ||
          !BlePeerStore::openV2(record, config.loraKeyHex.c_str())) {
        if (record.magic == BlePeerStore::MAGIC)
          gPeerMacFailures.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
    }
    out[i] = record;
  }
  if (migrated) (void)peerStoreSave(out);
  return true;
}

bool peerStoreSave(const PeerRecord in[PEER_MAX]) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (!in) return false;
  uint8_t blob[BlePeerStore::V2_BYTES * PEER_MAX] = {};
  for (size_t i = 0; i < PEER_MAX; ++i) {
    PeerRecord record = in[i];
    if (record.magic != BlePeerStore::MAGIC) continue;
    if (!BlePeerStore::sealV2(record, config.loraKeyHex.c_str())) return false;
    std::memcpy(blob + i * BlePeerStore::V2_BYTES, &record, sizeof(record));
  }

  Preferences prefs;
  if (!prefs.begin("ble_peer", false)) return false;
  const size_t written = prefs.putBytes("peers", blob, sizeof(blob));
  prefs.end();
  return written == sizeof(blob);
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
  const uint8_t* value = address.getValue();
  if (value) std::memcpy(out.bytes, value, sizeof(out.bytes));
  out.type = address.getType();
  return out;
}

uint64_t gatewayTimestampMs() {
  const time_t now = time(nullptr);
  if (now > 1700000000) return static_cast<uint64_t>(now) * 1000ULL;
  return static_cast<uint64_t>(millis());
}

String addressText(const SensorProtocol::BleAddress& address) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(17);
  for (int i = 5; i >= 0; --i) {
    if (i != 5) out += ':';
    out += hex[address.bytes[i] >> 4];
    out += hex[address.bytes[i] & 0x0F];
  }
  return out;
}

void notifyCallback(NimBLERemoteCharacteristic* characteristic,
                    uint8_t* data, size_t length, bool) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!gReader || !characteristic || !data || length != sizeof(SensorProtocol::SensorValue)) return;

  for (size_t i = 0; i < config.sensorMaxNodes; ++i) {
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
    SensorRegistry::Node nodeSnapshot{};
    if (!gReader->registry().snapshotNode(slot.nodeIndex, nodeSnapshot)) return;
    if (sensorIndex >= static_cast<int>(nodeSnapshot.sensorCount)) return;
    const SensorProtocol::SensorDescriptor descriptor =
        nodeSnapshot.descriptors[static_cast<size_t>(sensorIndex)];

    if (value.value < descriptor.min || value.value > descriptor.max)
      value.quality |= SensorProtocol::QUALITY_OUT_OF_RANGE;
    if (value.timestamp == 0) {
      value.timestamp = gatewayTimestampMs();
      value.quality |= SensorProtocol::QUALITY_TIMESTAMP_GATEWAY;
      // Sensor nodes intentionally leave timestamp=0 because they do not own
      // wall-clock time. The resulting timestamp is the gateway receive/
      // processing time, not the physical measurement instant.
      // This is the canonical Q-B03(A) contract until an explicit time-sync
      // protocol is introduced.
    } else if (value.timestamp < MIN_VALID_EPOCH_MS) {
      value.timestamp = gatewayTimestampMs();
      value.quality |= SensorProtocol::QUALITY_TIMESTAMP_INVALID |
                       SensorProtocol::QUALITY_TIMESTAMP_GATEWAY;
    }
    const uint32_t nowMs = millis();
    if (!gReader->registry().updateValue(slot.nodeIndex, value, nowMs)) return;
    SensorRegistry::Node liveNode{};
    if (!gReader->registry().snapshotNode(slot.nodeIndex, liveNode)) return;
    if (sensorIndex >= static_cast<int>(liveNode.sensorCount)) return;
    const SensorProtocol::SensorDescriptor& liveDescriptor =
        liveNode.descriptors[static_cast<size_t>(sensorIndex)];

    SensorReader::SensorSample sample{};
    sample.nodeId = nodeIdFromAddress(liveNode.address);
    sample.sensorId = value.id;
    sample.value = value.value;
    sample.quality = value.quality;
    sample.timestampMs = value.timestamp;
    sample.rssi = liveNode.rssi;
    std::strncpy(sample.nodeName, liveNode.name, sizeof(sample.nodeName) - 1);
    std::strncpy(sample.sensorName, liveDescriptor.name, sizeof(sample.sensorName) - 1);
    std::strncpy(sample.unit, liveDescriptor.unit, sizeof(sample.unit) - 1);
    // BLE callbacks must remain non-blocking: network/MQTT work is performed
    // by taskSensorForward after this sample has been queued.
    (void)gReader->enqueueSensorForLoRa(
        sample,
        (value.flags & SensorProtocol::FLAG_HAS_SOURCE_SEQUENCE) ? value.sourceSequence : 0,
        (liveDescriptor.flags & SensorProtocol::FLAG_HAS_SCHEMA_VERSION)
            ? liveDescriptor.schemaVersion : 0,
        SensorProtocol::FIRMWARE_VERSION);
    return;
  }
}

void cleanupSlot(ClientSlot& slot, bool disconnected) {
  SensorRegistry::Node nodeSnapshot{};
  if (gReader->registry().snapshotNode(slot.nodeIndex, nodeSnapshot)) {
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

bool writeSensorCommand(ClientSlot& slot, const SensorProtocol::CommandRequest& request) {
  if (!slot.command || !slot.command->canWrite()) return false;
  return slot.command->writeValue(reinterpret_cast<const uint8_t*>(&request),
                                  sizeof(request), true);
}

bool connectDevice(const NimBLEAdvertisedDevice* device) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (!gReader || !device) return false;
  const SensorProtocol::BleAddress advertised = toBleAddress(device->getAddress());
  SensorProtocol::BleAddress address{};
  bool addressIsRpa = false;
  bool bondedRpaPending = false;
  const char* advertisedName = device->haveName() ? device->getName().c_str() : nullptr;
  if (!gReader->resolvePeerIdentity(advertised, advertisedName, address, addressIsRpa)) {
    // For a bonded RPA, let NimBLE's controller/host resolving list determine
    // the identity during the connection. Do not create an application node
    // until the resolved identity is returned by the GAP connection descriptor.
    if (!isRpaAddress(advertised) || NimBLEDevice::getNumBonds() == 0) return false;
    address = advertised;
    addressIsRpa = true;
    bondedRpaPending = true;
  }
  if (!bondedRpaPending && config.blePairingEnabled && pairingBlocked(address)) return false;
  if (config.blePairingEnabled && !bondedRpaPending) {
    uint32_t passkey = 0;
    if (!gReader->getPeerPasskey(address, passkey)) return false;
    // NimBLE-Arduino exposes a process-wide client passkey; set it immediately
    // before secureConnection() so no connection can fall back to no security.
    NimBLEDevice::setSecurityPasskey(passkey);
    gBlePasskey = passkey;
  }
  size_t nodeIndex = SensorRegistry::MAX_SUPPORTED_NODES;
  if (!bondedRpaPending) {
    const int existingNode = gReader->registry().findNode(address);
    if (existingNode >= 0) {
      nodeIndex = static_cast<size_t>(existingNode);
      (void)gReader->registry().upsertNode(
          address, advertisedName, device->getRSSI(), millis(), nodeIndex);
    } else {
      if (addressIsRpa || gReader->registry().isFull() &&
          !gReader->registry().evictDisconnected(millis(), config.sensorNodeEvictionMs, nodeIndex)) {
        return false;
      }
      if (!gReader->registry().upsertNode(
              address, advertisedName, device->getRSSI(), millis(), nodeIndex)) {
        return false;
      }
    }
  }
  if (bondedRpaPending) gBlePasskey = 0;

  ClientSlot* slot = nullptr;
  for (size_t i = 0; i < config.sensorMaxNodes; ++i) {
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
  slot->client->setConnectTimeout(config.sensorConnectTimeoutMs);
  slot->client->setClientCallbacks(&gClientCallbacks, false);

  esp_task_wdt_reset();
  if (!slot->client->connect(device)) {
    (void)NimBLEDevice::deleteClient(slot->client);
    slot->client = nullptr;
    gReader->registry().markConnected(nodeIndex, false, millis());
    return false;
  }

  if (bondedRpaPending) {
    ble_gap_conn_desc desc{};
    if (ble_gap_conn_find(slot->client->getConnHandle(), &desc) != 0) {
      (void)NimBLEDevice::deleteClient(slot->client);
      slot->client = nullptr;
      return false;
    }
    SensorProtocol::BleAddress resolved{};
    std::memcpy(resolved.bytes, desc.peer_id_addr.val, sizeof(resolved.bytes));
    resolved.type = desc.peer_id_addr.type;

    bool bondedIdentity = false;
    for (size_t bond = 0; bond < NimBLEDevice::getNumBonds(); ++bond) {
      if (toBleAddress(NimBLEDevice::getBondedAddress(bond)) == resolved) {
        bondedIdentity = true;
        break;
      }
    }
    if (!bondedIdentity) {
      (void)NimBLEDevice::deleteClient(slot->client);
      slot->client = nullptr;
      return false;
    }

    address = resolved;
    const int existingNode = gReader->registry().findNode(address);
    if (existingNode >= 0) {
      nodeIndex = static_cast<size_t>(existingNode);
      (void)gReader->registry().upsertNode(
          address, advertisedName, device->getRSSI(), millis(), nodeIndex);
    } else {
      if (gReader->registry().isFull() &&
          !gReader->registry().evictDisconnected(
              millis(), config.sensorNodeEvictionMs, nodeIndex)) {
        (void)NimBLEDevice::deleteClient(slot->client);
        slot->client = nullptr;
        return false;
      }
      if (!gReader->registry().upsertNode(
              address, advertisedName, device->getRSSI(), millis(), nodeIndex)) {
        (void)NimBLEDevice::deleteClient(slot->client);
        slot->client = nullptr;
        return false;
      }
    }
    slot->nodeIndex = nodeIndex;
    if (addressIsRpa) (void)gReader->registry().setLastRpa(nodeIndex, advertised);
  }

  esp_task_wdt_reset();
  if (config.blePairingEnabled || config.blePairingPolicy == 1) {
    if (!slot->client->secureConnection()) {
      pairingFailure(address);
      (void)NimBLEDevice::deleteClient(slot->client);
      *slot = {};
      gReader->registry().markConnected(nodeIndex, false, millis());
      return false;
    }
    pairingSuccess(address);
    if (addressIsRpa) (void)gReader->recordPeerRpa(address, advertised);
  } else if ((config.sensorRequireEncryption || config.blePairingPolicy == 1) &&
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
  slot->command = service->getCharacteristic(NimBLEUUID(SensorProtocol::COMMAND_UUID));
  slot->commandResponse = service->getCharacteristic(
      NimBLEUUID(SensorProtocol::COMMAND_RESPONSE_UUID));
  if (!slot->request || !slot->descriptor || !slot->value ||
      (!slot->request->canWrite() && !slot->request->canWriteNoResponse()) ||
      !slot->descriptor->canRead() ||
      (!slot->value->canNotify() && !slot->value->canIndicate())) {
    (void)NimBLEDevice::deleteClient(slot->client);
    *slot = {};
    gReader->registry().markConnected(nodeIndex, false, millis());
    return false;
  }

  if (slot->command && slot->commandResponse) {
    if (!slot->command->canWrite() ||
        (!slot->commandResponse->canRead() &&
         !slot->commandResponse->canNotify() &&
         !slot->commandResponse->canIndicate())) {
      slot->command = nullptr;
      slot->commandResponse = nullptr;
    } else if (slot->commandResponse->canNotify() || slot->commandResponse->canIndicate()) {
      (void)slot->commandResponse->subscribe(
          slot->commandResponse->canNotify(), &gCommandResponseCallbacks);
    }
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
  if (response.size() < sizeof(SensorProtocol::SensorDescriptorResponse)) {
    cleanupSlot(*slot, true);
    return false;
  }

  SensorProtocol::SensorDescriptorResponse first{};
  std::memset(&first, 0, sizeof(first));
  std::memcpy(&first, response.data(), response.size() < sizeof(first) ? response.size() : sizeof(first));
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
    if (item.size() < sizeof(SensorProtocol::SensorDescriptorResponse)) {
      cleanupSlot(*slot, true);
      return false;
    }
    SensorProtocol::SensorDescriptorResponse decoded{};
    std::memcpy(&decoded, item.data(),
                item.size() < sizeof(decoded) ? item.size() : sizeof(decoded));
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
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (!config.sensorReaderEnabled) return false;
  if (initialized_) return true;
  (void)peerMutexLock();
  peerMutexUnlock();
  if (!NimBLEDevice::isInitialized() &&
      !NimBLEDevice::init(std::string(gatewayName.c_str()))) return false;
  if (config.sensorRequireEncryption && !config.blePairingEnabled) {
    // The current sensor-node contract requires an authenticated connection.
    // Refuse an incompatible encryption-only configuration rather than
    // silently negotiating Just Works and failing the node-side auth gate.
    Serial.println("ERROR: SENSOR_REQUIRE_ENCRYPTION requires BLE pairing/MITM");
    return false;
  }
  // NimBLE's bond/IRK resolver remains the authoritative resolver for bonded
  // peers. Manual IRK resolution below is only a fallback for unbonded peers.
  NimBLEDevice::setSecurityAuth(true /* bonding */, true /* MITM */, true /* SC */);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  (void)NimBLEDevice::setMTU(Config::SENSOR_MTU_VALUE);
  gScan = NimBLEDevice::getScan();
  if (!gScan) return false;
  gScan->setActiveScan(Config::SENSOR_ACTIVE_SCAN_VALUE);
  gScan->setInterval(config.sensorScanIntervalMs);
  gScan->setWindow(config.sensorScanWindowMs);
  gScan->setMaxResults(static_cast<uint8_t>(config.sensorMaxNodes));
  sensorQueue_ = xQueueCreateStatic(Config::SENSOR_LORA_QUEUE_DEPTH, sizeof(QueuedSample),
                                    sensorQueueStorage_, &sensorQueueStruct_);
  if (!sensorQueue_) return false;
  gReader = this;
  initialized_ = true;
  Serial.printf("SENSOR: reader enabled, NimBLE=%s maxNodes=%u maxSensors=%u\n",
                NimBLEDevice::getVersion(),
                static_cast<unsigned>(config.sensorMaxNodes),
                static_cast<unsigned>(Config::SENSOR_MAX_SENSORS_PER_NODE_VALUE));
  return true;
}

void SensorReader::task() {
  RuntimeConfig config;
  if (!initialized_ || !gScan || !configSnapshot(config)) return;
  if (!config.sensorReaderEnabled) return;

  // BLE scan parameters are runtime policy. Re-apply them here so changes
  // made through the WebUI take effect without rebooting the node.
  const size_t maxNodes = config.sensorMaxNodes;
  gScan->setInterval(config.sensorScanIntervalMs);
  gScan->setWindow(config.sensorScanWindowMs);
  gScan->setMaxResults(static_cast<uint8_t>(maxNodes));

  const uint32_t nowMs = millis();
  for (auto& slot : gSlots) {
    if (!slot.inUse || !slot.commandPending) continue;
    if (slot.commandResponseReady) {
      slot.commandPending = false;
      continue;
    }
    if (static_cast<uint32_t>(nowMs - slot.pendingCommandSentMs) < 1000U) continue;
    if (slot.pendingCommandRetries >= 3) {
      slot.commandPending = false;
      continue;
    }
    if (writeSensorCommand(slot, slot.pendingCommand)) {
      ++slot.pendingCommandRetries;
      slot.pendingCommandSentMs = nowMs;
    } else {
      slot.commandPending = false;
    }
  }

  // A runtime reduction of maxNodes must also release slots above the new
  // limit; otherwise connected clients can remain alive outside the configured
  // resource bound indefinitely.
  for (size_t i = maxNodes; i < SensorRegistry::MAX_SUPPORTED_NODES; ++i) {
    if (gSlots[i].inUse) cleanupSlot(gSlots[i], true);
  }

  // WebUI actions are consumed by the BLE task; the HTTP handler never tears
  // down a NimBLE client or mutates the registry directly.
  for (size_t nodeIndex = 0; nodeIndex < maxNodes; ++nodeIndex) {
    if (forgetRequested_[nodeIndex].exchange(false, std::memory_order_acq_rel)) {
      for (size_t i = 0; i < maxNodes; ++i) {
        if (gSlots[i].inUse && gSlots[i].nodeIndex == nodeIndex) cleanupSlot(gSlots[i], true);
      }
      (void)registry_.forgetNode(nodeIndex);
      refreshRequested_[nodeIndex].store(false, std::memory_order_release);
    } else if (refreshRequested_[nodeIndex].exchange(false, std::memory_order_acq_rel)) {
      for (size_t i = 0; i < maxNodes; ++i) {
        if (gSlots[i].inUse && gSlots[i].nodeIndex == nodeIndex) cleanupSlot(gSlots[i], true);
      }
      refreshRequested_[nodeIndex] = false;
    }
  }

  for (size_t i = 0; i < maxNodes; ++i) {
    ClientSlot& slot = gSlots[i];
    if (!slot.inUse) continue;
    if (!slot.client || !slot.client->isConnected() || slot.descriptorRefreshRequested)
      cleanupSlot(slot, true);
  }

  size_t active = 0;
  for (size_t i = 0; i < maxNodes; ++i) active += gSlots[i].inUse ? 1U : 0U;
  if (active < maxNodes) {
    esp_task_wdt_reset();
    const NimBLEScanResults results = gScan->getResults(config.sensorScanDurationMs, false);
    for (int i = 0; i < results.getCount() && active < maxNodes; ++i) {
      esp_task_wdt_reset();
      const NimBLEAdvertisedDevice* device = results.getDevice(static_cast<uint32_t>(i));
      if (!device || !device->isAdvertisingService(NimBLEUUID(SensorProtocol::SERVICE_UUID))) continue;
      const SensorProtocol::BleAddress advertised = toBleAddress(device->getAddress());
      SensorProtocol::BleAddress identity{};
      bool isRpa = false;
      if (!resolvePeerIdentity(advertised,
                               device->haveName() ? device->getName().c_str() : nullptr,
                               identity, isRpa)) continue;
      const int existingNode = registry_.findNode(identity);
      if (existingNode >= 0) {
        SensorRegistry::Node nodeSnapshot{};
        if (registry_.snapshotNode(static_cast<size_t>(existingNode), nodeSnapshot) &&
            nodeSnapshot.connected) continue;
      }
      if (connectDevice(device)) ++active;
    }
    gScan->clearResults();
  }

  vTaskDelay(pdMS_TO_TICKS( max<uint32_t>(100, config.sensorScanIntervalMs / 2U) ));
}

bool SensorReader::enqueueSensorForLoRa(const SensorSample& sample,
                                           uint32_t sourceSequence,
                                           uint8_t schemaVersion,
                                           uint32_t firmwareVersion) {
  if (!sensorQueue_) return false;
  QueuedSample queued{};
  queued.sample = sample;
  queued.sourceSequence = sourceSequence;
  queued.schemaVersion = schemaVersion;
  queued.firmwareVersion = firmwareVersion;
  if (xQueueSend(sensorQueue_, &queued, 0) == pdTRUE) return true;
  if (queuePolicy() == SampleQueuePolicy::DROP_OLDEST) {
    QueuedSample discarded{};
    if (xQueueReceive(sensorQueue_, &discarded, 0) == pdTRUE) {
      droppedSamples_.fetch_add(1, std::memory_order_relaxed);
      if (xQueueSend(sensorQueue_, &queued, 0) == pdTRUE) return true;
    }
    droppedSamples_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  droppedSamples_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool SensorReader::popSensorForLoRa(SensorSample& sample, TickType_t timeout,
                                     uint32_t* sourceSequence,
                                     uint8_t* schemaVersion,
                                     uint32_t* firmwareVersion) {
  if (!sensorQueue_) return false;
  QueuedSample queued{};
  if (xQueueReceive(sensorQueue_, &queued, timeout) != pdTRUE) return false;
  sample = queued.sample;
  if (sourceSequence) *sourceSequence = queued.sourceSequence;
  if (schemaVersion) *schemaVersion = queued.schemaVersion;
  if (firmwareVersion) *firmwareVersion = queued.firmwareVersion;
  return true;
}

bool SensorReader::peekSensorForLoRa(SensorSample& sample,
                                     uint32_t* sourceSequence,
                                     uint8_t* schemaVersion,
                                     uint32_t* firmwareVersion) const {
  if (!sensorQueue_) return false;
  QueuedSample queued{};
  if (xQueuePeek(sensorQueue_, &queued, 0) != pdTRUE) return false;
  sample = queued.sample;
  if (sourceSequence) *sourceSequence = queued.sourceSequence;
  if (schemaVersion) *schemaVersion = queued.schemaVersion;
  if (firmwareVersion) *firmwareVersion = queued.firmwareVersion;
  return true;
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
  forgetRequested_[nodeIndex].store(true, std::memory_order_release);
  return true;
}

bool SensorReader::requestRefreshNode(size_t nodeIndex) {
  SensorRegistry::Node node{};
  if (nodeIndex >= SensorRegistry::MAX_SUPPORTED_NODES || !registry_.snapshotNode(nodeIndex, node)) return false;
  refreshRequested_[nodeIndex].store(true, std::memory_order_release);
  return true;
}

uint32_t SensorReader::peerMacFailures() const { return gPeerMacFailures.load(std::memory_order_relaxed); }

bool SensorReader::isEnabled() const {
  RuntimeConfig config{};
  if (!configSnapshot(config)) return false;
  return config.sensorReaderEnabled;
}

bool SensorReader::hasConnectedNode() const {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  for (size_t i = 0; i < config.sensorMaxNodes; ++i) {
    SensorRegistry::Node nodeSnapshot{};
    if (registry_.snapshotNode(i, nodeSnapshot) && nodeSnapshot.connected) return true;
  }
  return false;
}


bool SensorReader::sendSensorCommand(size_t nodeIndex, uint8_t commandId,
                                         uint16_t sensorId, uint32_t argument) {
  if (nodeIndex >= SensorRegistry::MAX_SUPPORTED_NODES || commandId == 0) return false;
  ClientSlot* slot = nullptr;
  for (auto& candidate : gSlots) {
    if (candidate.inUse && candidate.nodeIndex == nodeIndex) {
      slot = &candidate;
      break;
    }
  }
  if (!slot || !slot->command || !slot->commandResponse) return false;
  SensorRegistry::Node node{};
  if (!registry_.snapshotNode(nodeIndex, node)) return false;
  if (!peerMutexLock()) return false;
  PeerRecord peers[PEER_MAX]{};
  bool ok = peerStoreLoad(peers);
  if (!ok) {
    peerMutexUnlock();
    return false;
  }
  size_t peerIndex = PEER_MAX;
  for (size_t i = 0; i < PEER_MAX; ++i) {
    if (peers[i].magic == BlePeerStore::MAGIC &&
        peers[i].identity == node.address) {
      peerIndex = i;
      break;
    }
  }
  if (peerIndex == PEER_MAX) {
    peerMutexUnlock();
    return false;
  }
  uint32_t sequence = peers[peerIndex].lastCommandSequence;
  if (++sequence == 0) sequence = 1;
  peers[peerIndex].lastCommandSequence = sequence;
  ok = peerStoreSave(peers);
  peerMutexUnlock();
  if (!ok) return false;

  SensorProtocol::CommandRequest request{};
  request.commandId = commandId;
  request.sequence = sequence;
  request.sensorId = sensorId;
  request.argument = argument;
  slot->pendingCommand = request;
  slot->pendingCommandSequence = sequence;
  slot->pendingCommandId = commandId;
  slot->pendingCommandRetries = 0;
  slot->pendingCommandSentMs = millis();
  slot->commandResponseReady = false;
  slot->commandCompleted = false;
  if (!writeSensorCommand(*slot, request)) {
    slot->commandPending = false;
    return false;
  }
  slot->pendingCommandRetries = 1;
  slot->commandPending = true;
  return true;
}

bool SensorReader::getSensorCommandResponse(
    size_t nodeIndex, SensorProtocol::CommandResponse& out) const {
  if (nodeIndex >= SensorRegistry::MAX_SUPPORTED_NODES) return false;
  const ClientSlot* slot = nullptr;
  for (const auto& candidate : gSlots) {
    if (candidate.inUse && candidate.nodeIndex == nodeIndex) {
      slot = &candidate;
      break;
    }
  }
  if (!slot || !slot->commandCompleted) return false;
  out = slot->lastCommandResponse;
  return true;
}

bool SensorReader::setPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t passkey) {
  if (address.bytes[0] == 0 && address.bytes[1] == 0 && address.bytes[2] == 0 &&
      address.bytes[3] == 0 && address.bytes[4] == 0 && address.bytes[5] == 0) return false;
  if (passkey < 100000U || passkey > 999999U || !peerMutexLock()) return false;

  PeerRecord peers[PEER_MAX]{};
  if (!peerStoreLoad(peers)) { peerMutexUnlock(); return false; }

  size_t slot = PEER_MAX;
  for (size_t i = 0; i < PEER_MAX; ++i) {
    if (peers[i].magic == BlePeerStore::MAGIC && peers[i].identity == address) {
      slot = i;
      break;
    }
  }
  if (slot == PEER_MAX) {
    for (size_t i = 0; i < PEER_MAX; ++i) {
      if (peers[i].magic != BlePeerStore::MAGIC) {
        slot = i;
        break;
      }
    }
  }
  if (slot == PEER_MAX) { peerMutexUnlock(); return false; }

  PeerRecord& p = peers[slot];
  const String oldName = String(p.name);
  const auto oldLastRpa = p.lastRpa;
  uint8_t oldIrk[16] = {};
  std::memcpy(oldIrk, p.irk, sizeof(oldIrk));
  p = {};
  p.magic = BlePeerStore::MAGIC;
  p.version = BlePeerStore::VERSION;
  BlePeerStore::setPasskey(p, passkey);
  p.identity = address;
  p.lastRpa = oldLastRpa;
  std::memcpy(p.irk, oldIrk, sizeof(p.irk));
  p.updatedEpoch = static_cast<uint32_t>(
      time(nullptr) > 1700000000 ? time(nullptr) : 0);

  const int existing = registry_.findNode(address);
  if (existing >= 0) {
    SensorRegistry::Node n{};
    if (registry_.snapshotNode(static_cast<size_t>(existing), n)) {
      p.lastRpa = n.lastRPA;
      std::strncpy(p.name, n.name, sizeof(p.name) - 1);
    }
  } else {
    std::strncpy(p.name, oldName.c_str(), sizeof(p.name) - 1);
  }

  const bool ok = peerStoreSave(peers);
  peerMutexUnlock();
  return ok;
}

bool SensorReader::setPeerIrk(const SensorProtocol::BleAddress& address, const uint8_t irk[16]) {
  if (!irk || !peerMutexLock()) return false;
  PeerRecord peers[PEER_MAX]{};
  if (!peerStoreLoad(peers)) { peerMutexUnlock(); return false; }

  for (auto& p : peers) {
    if (p.magic == BlePeerStore::MAGIC && p.identity == address) {
      std::memcpy(p.irk, irk, sizeof(p.irk));
      p.updatedEpoch = static_cast<uint32_t>(
          time(nullptr) > 1700000000 ? time(nullptr) : p.updatedEpoch);
      const bool ok = peerStoreSave(peers);
      peerMutexUnlock();
      return ok;
    }
  }
  peerMutexUnlock();
  return false;
}

bool SensorReader::forgetPeerPasskey(const SensorProtocol::BleAddress& address) {
  if (!peerMutexLock()) return false;
  PeerRecord peers[PEER_MAX]{};
  if (!peerStoreLoad(peers)) { peerMutexUnlock(); return false; }
  bool found = false;
  for (auto& p : peers) {
    if (p.magic == BlePeerStore::MAGIC && p.identity == address) {
      p = {};
      found = true;
    }
  }
  const bool ok = found && peerStoreSave(peers);
  peerMutexUnlock();
  return ok;
}

bool SensorReader::getPeerPasskey(const SensorProtocol::BleAddress& address, uint32_t& passkey) const {
  passkey = 0;
  if (!peerMutexLock()) return false;
  PeerRecord peers[PEER_MAX]{};
  if (!peerStoreLoad(peers)) { peerMutexUnlock(); return false; }
  for (auto& p : peers) {
    if (p.magic == BlePeerStore::MAGIC && p.identity == address &&
        peerFresh(p.updatedEpoch)) {
      passkey = BlePeerStore::passkey(p);
      peerMutexUnlock();
      return true;
    }
  }
  peerMutexUnlock();
  return false;
}

bool SensorReader::resolvePeerIdentity(const SensorProtocol::BleAddress& advertised,
                                       const char*,
                                       SensorProtocol::BleAddress& identity,
                                       bool& isRpaOut) const {
  identity = advertised;
  isRpaOut = false;

  // For bonded peers, NimBLE's bond/IRK resolver is the authoritative BLE
  // security mechanism. Application state is only used after a stable identity
  // has already been associated with the peer.
  const int direct = registry_.findNode(advertised);
  if (direct >= 0) {
    SensorRegistry::Node n{};
    if (registry_.snapshotNode(static_cast<size_t>(direct), n)) identity = n.address;
    isRpaOut = n.hasRPA && n.lastRPA == advertised;
    return true;
  }

  if (!isRpaAddress(advertised) || !peerMutexLock()) return !isRpaAddress(advertised);
  PeerRecord peers[PEER_MAX]{};
  if (!peerStoreLoad(peers)) { peerMutexUnlock(); return false; }

  for (auto& p : peers) {
    if (p.magic != BlePeerStore::MAGIC || !peerFresh(p.updatedEpoch)) continue;
    if (!hasIrk(p.irk)) continue;
    if (BlePeerStore::matchesRpa(advertised, p.irk)) {
      identity = p.identity;
      isRpaOut = true;
      peerMutexUnlock();
      return true;
    }
  }
  peerMutexUnlock();
  return false; // never create a new node for an unresolved RPA.
}

bool SensorReader::recordPeerRpa(const SensorProtocol::BleAddress& identity,
                                 const SensorProtocol::BleAddress& rpa) {
  if (!isRpaAddress(rpa) || !peerMutexLock()) return false;
  PeerRecord peers[PEER_MAX]{};
  if (!peerStoreLoad(peers)) { peerMutexUnlock(); return false; }
  for (auto& p : peers) {
    if (p.magic == BlePeerStore::MAGIC && p.identity == identity) {
      p.lastRpa = rpa;
      p.updatedEpoch = static_cast<uint32_t>(
          time(nullptr) > 1700000000 ? time(nullptr) : p.updatedEpoch);
      const bool ok = peerStoreSave(peers);
      peerMutexUnlock();
      return ok;
    }
  }
  peerMutexUnlock();
  return false;
}

String SensorReader::peersJson() const {
  String j = "[";
  if (!peerMutexLock()) return j + "]";
  PeerRecord peers[PEER_MAX]{};
  if (!peerStoreLoad(peers)) { peerMutexUnlock(); return j + "]"; }
  bool first = true;
  for (const auto& p : peers) {
    if (p.magic != BlePeerStore::MAGIC || !peerFresh(p.updatedEpoch)) continue;
    if (!first) j += ',';
    first = false;
    String addr = addressText(p.identity);
    const uint32_t key = BlePeerStore::passkey(p);
    j += "{\"addr\":\"" + addr + "\",\"passkey\":\"****" +
         String(key).substring(4) + "\",\"irkConfigured\":" +
         String(hasIrk(p.irk) ? "true" : "false") + "}";
  }
  peerMutexUnlock();
  return j + "]";
}

#else

bool SensorReader::begin(const String&) { return false; }
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
bool SensorReader::enqueueSensorForLoRa(const SensorSample&, uint32_t, uint8_t, uint32_t) { return false; }
uint32_t SensorReader::droppedSamples() const { return 0; }
uint8_t SensorReader::queueDepth() const { return 0; }
uint32_t SensorReader::peerMacFailures() const { return 0; }
bool SensorReader::sendSensorCommand(size_t, uint8_t, uint16_t, uint32_t) { return false; }
bool SensorReader::getSensorCommandResponse(
    size_t, SensorProtocol::CommandResponse&) const { return false; }
bool SensorReader::setPeerPasskey(const SensorProtocol::BleAddress&, uint32_t) { return false; }
bool SensorReader::setPeerIrk(const SensorProtocol::BleAddress&, const uint8_t[16]) { return false; }
bool SensorReader::forgetPeerPasskey(const SensorProtocol::BleAddress&) { return false; }
bool SensorReader::getPeerPasskey(const SensorProtocol::BleAddress&, uint32_t&) const { return false; }
bool SensorReader::resolvePeerIdentity(const SensorProtocol::BleAddress& a, const char*, SensorProtocol::BleAddress& i, bool& r) const { i = a; r = false; return false; }
bool SensorReader::recordPeerRpa(const SensorProtocol::BleAddress&, const SensorProtocol::BleAddress&) { return false; }
String SensorReader::peersJson() const { return "[]"; }
bool SensorReader::popSensorForLoRa(SensorSample&, TickType_t, uint32_t*, uint8_t*, uint32_t*) { return false; }
bool SensorReader::peekSensorForLoRa(SensorSample&, uint32_t*, uint8_t*, uint32_t*) const { return false; }
bool SensorReader::snapshotNodes(SensorNodeSnapshot*, size_t, size_t& count) const { count = 0; return false; }
bool SensorReader::snapshotNode(size_t, SensorRegistry::Node&) const { return false; }
bool SensorReader::requestForgetNode(size_t) { return false; }
bool SensorReader::requestRefreshNode(size_t) { return false; }
void SensorReader::task() {}
bool SensorReader::isEnabled() const { return false; }
bool SensorReader::hasConnectedNode() const { return false; }

#endif
