#include "BleSensorServer.h"
#include "Config.h"
#include "SensorProtocol.h"
#include "NimBLEDevice.h"
#include <Preferences.h>
#include <esp_system.h>
#include <cstring>

namespace {
BleSensorServer* gServer = nullptr;
uint32_t gPasskey = 0;

uint32_t loadOrCreatePasskey() {
  // A per-device random passkey is preferable to a derivation from the public
  // BLE address. It is printed only on the local provisioning console.
  Preferences prefs;
  if (!prefs.begin("ble-sec", false)) return 0;
  uint32_t key = prefs.getUInt("passkey", 0);
  if (key < 100000U || key > 999999U) {
    key = 100000U + (esp_random() % 900000U);
    if (!prefs.putUInt("passkey", key)) {
      prefs.end();
      return 0;
    }
  }
  prefs.end();
  return key;
}

class ServerSecurityCallbacks final : public NimBLEServerCallbacks {
public:
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    // Do not mark the peer usable until link security has completed. The
    // notification characteristic has no encrypted property in NimBLE-Arduino,
    // so connection state itself is the final gate against pre-auth telemetry.
    if (gServer) gServer->setClientConnected(false);
    if (!connInfo.isEncrypted()) {
      const bool started = NimBLEDevice::startSecurity(connInfo.getConnHandle());
      if (!started) Serial.println("WARN: failed to start BLE security");
    }
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    if (gServer) gServer->setClientConnected(false);
  }

  uint32_t onPassKeyDisplay() override {
    Serial.printf("BLE pairing passkey: %06lu\n", static_cast<unsigned long>(gPasskey));
    return gPasskey;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    const bool secure = connInfo.isEncrypted() && connInfo.isAuthenticated();
    Serial.printf("BLE authentication %s: %s\n",
                  secure ? "OK" : "FAILED",
                  connInfo.getAddress().toString().c_str());
    if (gServer) gServer->setClientConnected(secure);
    if (!secure && gBleServer) {
      gBleServer->disconnect(connInfo);
    }
  }
};

class DescriptorRequestCallbacks final : public NimBLECharacteristicCallbacks {
public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    if (!gServer || !characteristic) return;
    const std::string raw = characteristic->getValue();
    if (raw.size() != 3U) return;
    const uint8_t* request = reinterpret_cast<const uint8_t*>(raw.data());
    if (request[0] != SensorProtocol::PROTOCOL_VERSION ||
        request[1] != SensorProtocol::DESCRIPTOR_REQUEST_LIST) return;
    gServer->updateDescriptorResponse(request[2]);
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    if (gServer) gServer->handleCommand(characteristic, connInfo);
  }
};

ServerSecurityCallbacks gSecurityCallbacks;
DescriptorRequestCallbacks gRequestCallbacks;
CommandCallbacks gCommandCallbacks;
}  // namespace

bool BleSensorServer::begin(const String& nodeName, SensorRegistry& registry) {
  registry_ = &registry;
  nodeName_ = nodeName.length() ? nodeName : SensorNodeConfig::DEFAULT_NODE_NAME;

  if (!NimBLEDevice::init(nodeName_.c_str())) return false;
  // G13/G15: sensor nodes use a stable public identity address. This API is
  // assumed available in NimBLE-Arduino 2.5.x; verify when changing versions.
  (void)NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
  NimBLEDevice::setMTU(128);
  NimBLEDevice::setSecurityAuth(SensorNodeConfig::BLE_BONDING,
                                SensorNodeConfig::BLE_MITM,
                                SensorNodeConfig::BLE_SECURE_CONNECTIONS);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  gPasskey = loadOrCreatePasskey();
  if (gPasskey == 0) return false;
  NimBLEDevice::setSecurityPasskey(gPasskey);

  server_ = NimBLEDevice::createServer();
  if (!server_) return false;
  server_->setCallbacks(&gSecurityCallbacks);
  gBleServer = server_;
  server_->advertiseOnDisconnect(true);

  service_ = server_->createService(SensorProtocol::SERVICE_UUID);
  if (!service_) return false;

  request_ = service_->createCharacteristic(
      SensorProtocol::DESCRIPTOR_REQUEST_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC,
      3);
  descriptor_ = service_->createCharacteristic(
      SensorProtocol::DESCRIPTOR_DATA_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC,
      sizeof(SensorProtocol::SensorDescriptorResponse));
  value_ = service_->createCharacteristic(
      SensorProtocol::SENSOR_VALUE_UUID,
      NIMBLE_PROPERTY::NOTIFY,
      sizeof(SensorProtocol::SensorValue));
  command_ = service_->createCharacteristic(
      SensorProtocol::COMMAND_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
      sizeof(SensorProtocol::CommandRequest));
  commandResponse_ = service_->createCharacteristic(
      SensorProtocol::COMMAND_RESPONSE_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC,
      sizeof(SensorProtocol::CommandResponse));
  if (!request_ || !descriptor_ || !value_ || !command_ || !commandResponse_) return false;
  request_->setCallbacks(&gRequestCallbacks);
  command_->setCallbacks(&gCommandCallbacks);

  SensorProtocol::SensorDescriptorResponse empty{};
  empty.total = static_cast<uint8_t>(registry_->count());
  descriptor_->setValue(reinterpret_cast<const uint8_t*>(&empty), sizeof(empty));
  service_->start();
  gServer = this;
  startAdvertising();
  running_ = true;
  Serial.printf("BLE sensor node ready: %s, passkey %06lu\n",
                nodeName_.c_str(), static_cast<unsigned long>(gPasskey));
  return true;
}

void BleSensorServer::handleCommand(NimBLECharacteristic* characteristic,
                                      NimBLEConnInfo& connInfo) {
  if (!characteristic || !commandResponse_ || !commandHandler_ ||
      !connInfo.isEncrypted() || !connInfo.isAuthenticated()) return;
  const std::string raw = characteristic->getValue();
  if (raw.size() != sizeof(SensorProtocol::CommandRequest)) return;
  SensorProtocol::CommandRequest request{};
  std::memcpy(&request, raw.data(), sizeof(request));
  SensorProtocol::CommandResponse response{};
  response.commandId = request.commandId;
  response.sequence = request.sequence;

  Preferences prefs;
  if (!prefs.begin("ble-cmd", false)) return;
  char key[15] = {};
  const std::string address = connInfo.getAddress().toString();
  uint32_t addressHash = 2166136261UL;
  for (unsigned char c : address) { addressHash ^= c; addressHash *= 16777619UL; }
  std::snprintf(key, sizeof(key), "seq_%08lX",
                static_cast<unsigned long>(addressHash));
  const uint32_t lastSequence = prefs.getUInt(key, 0);
  if (request.sequence == 0 || request.sequence <= lastSequence) {
    // A duplicate is an acknowledged replay: do not execute it twice.
    response.result = request.sequence == lastSequence ? 0 : 1;
    response.errorCode = request.sequence == lastSequence ? 0 : 1;
    prefs.end();
    commandResponse_->setValue(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    commandResponse_->notify();
    return;
  }

  // Persist the command sequence before invoking any side effect. A failed
  // NVS write must fail closed so reboot cannot replay an already executed command.
  if (prefs.putUInt(key, request.sequence) != sizeof(uint32_t)) {
    response.result = 1;
    response.errorCode = SensorProtocol::COMMAND_ERROR_PERSISTENCE_FAILED;
    prefs.end();
    commandResponse_->setValue(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    commandResponse_->notify();
    return;
  }

  if (!commandHandler_(request, response)) {
    response.result = 1;
    if (response.errorCode == 0) response.errorCode = 2;
  }
  prefs.end();
  commandResponse_->setValue(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
  commandResponse_->notify();
}

void BleSensorServer::startAdvertising() {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->stop();
  advertising->setName(nodeName_.c_str());
  advertising->addServiceUUID(SensorProtocol::SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->start();
  advertisingStartedAtMs_ = millis();
}

void BleSensorServer::updateDescriptorResponse(uint8_t index) {
  if (!registry_ || !descriptor_) return;
  SensorProtocol::SensorDescriptorResponse response{};
  response.index = index;
  response.total = static_cast<uint8_t>(registry_->count());
  response.version = SensorProtocol::PROTOCOL_VERSION;
  response.magic = SensorProtocol::DESCRIPTOR_MAGIC;
  const SensorProtocol::SensorDescriptor* descriptor = registry_->descriptor(index);
  if (descriptor) response.descriptor = *descriptor;
  descriptor_->setValue(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
}

void BleSensorServer::notifyValues() {
  if (!registry_ || !value_ || !clientConnected_) return;
  for (size_t i = 0; i < registry_->count(); ++i) {
    const SensorProtocol::SensorValue* value = registry_->value(i);
    if (!value || !SensorProtocol::validValue(*value)) continue;
    SensorProtocol::SensorValue wire = *value;
    value_->notify(reinterpret_cast<const uint8_t*>(&wire), sizeof(wire));
  }
}

void BleSensorServer::task() {
  if (!running_) return;
  const uint32_t now = millis();
  if (now - lastNotifyMs_ >= SensorNodeConfig::SENSOR_NOTIFICATION_PERIOD_MS) {
    lastNotifyMs_ = now;
    notifyValues();
  }

  if (SensorNodeConfig::DEEP_SLEEP_ENABLED &&
      now - advertisingStartedAtMs_ >= SensorNodeConfig::DEEP_SLEEP_ADVERTISE_MS &&
      !clientConnected_) {
    esp_sleep_enable_timer_wakeup(SensorNodeConfig::DEEP_SLEEP_WAKE_US);
    Serial.println("BLE deep sleep: no client connected");
    Serial.flush();
    esp_deep_sleep_start();
  }
}
