#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include "Config.h"
#include "ProfileManager.h"
#include "OtaApManager.h"
#include "ProfileSensors.h"
#include "SensorRegistry.h"
#include "BleSensorServer.h"
#include "SensorDriverRegistry.h"
#include "RfidReader.h"
#include "SensorProtocol.h"

namespace {
SensorRegistry registry;
SensorDriverRegistry driverRegistry;
BleSensorServer bleServer;
ProfileManager profileManager;
ProfileSensors profileSensors;
OtaApManager otaApManager;
RfidReader rfidReader;
Preferences prefs;
String nodeName;
// OTA is served only through the local AP managed by OtaApManager.
// Green-field provisioning persists only the local OTA password.
String otaPassword;
constexpr size_t OTA_PASSWORD_MIN_LEN = 12;
constexpr size_t OTA_PASSWORD_MAX_LEN = 64;

bool validOtaPassword(const String& value) {
  if (value.length() < OTA_PASSWORD_MIN_LEN ||
      value.length() > OTA_PASSWORD_MAX_LEN) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c < 0x21 || c > 0x7E) return false;
  }
  return true;
}

bool saveOtaProvisioning(const String& otaPass) {
  Preferences otaPrefs;
  if (!otaPrefs.begin("ota", false)) return false;
  const bool ok = otaPrefs.putString("opass", otaPass) > 0;
  otaPrefs.end();
  return ok;
}

void onLongPressToggleAp() {
  Serial.println("BUTTON: long press -> toggle OTA AP");
  otaApManager.toggleAp();
}

bool onWebProfileChange(uint8_t rawProfile) {
  if (rawProfile >= ProfileConfig::PROFILE_COUNT) {
    Serial.printf("WEBUI: invalid profile %u\n",
                  static_cast<unsigned>(rawProfile));
    return false;
  }

  const auto profile = static_cast<ProfileConfig::Profile>(rawProfile);
  if (!profileManager.setProfile(profile)) {
    Serial.println("WEBUI: profile NVS update failed");
    return false;
  }

  Serial.printf("WEBUI: profile changed to %s; reboot required\n",
                ProfileConfig::profileName(profile));
  return true;
}


bool validNodeNameLength(const String& value, size_t maxBytes) {
  return value.length() > 0 && value.length() <= maxBytes;
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help");
  Serial.println("  show");
  Serial.println("  name <node-name>        set node name (max 24 bytes)");
  Serial.println("  ota password <secret>   stage OTA password (min 12 chars)");
  Serial.println("  ota save                save OTA provisioning and reboot");
  Serial.println("  save                    save provisioning and reboot");
}
void loadProvisioning() {
  if (!prefs.begin("sensor", false)) {
    nodeName = SensorNodeConfig::DEFAULT_NODE_NAME;
    return;
  }
  nodeName = prefs.getString("name", SensorNodeConfig::DEFAULT_NODE_NAME);
  prefs.end();
}

void saveProvisioning() {
  if (!prefs.begin("sensor", false)) {
    Serial.println("ERROR: NVS open failed");
    return;
  }
  if (!validNodeNameLength(nodeName, SensorProtocol::MAX_NODE_NAME_BYTES - 1)) {
    Serial.println("ERROR: invalid node name length");
    prefs.end();
    return;
  }
  const bool ok = prefs.putString("name", nodeName) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("ERROR: NVS write failed; provisioning not committed");
    return;
  }
  Serial.println("Provisioning saved. Rebooting...");
  delay(100);
  ESP.restart();
}

void showProvisioning() {
  Serial.printf("name=%s profile=%u drivers=%u sensors=%u rfid=%s\n",
                nodeName.c_str(),
                static_cast<unsigned>(profileManager.activeProfile()),
                static_cast<unsigned>(driverRegistry.count()),
                static_cast<unsigned>(registry.count()),
                rfidReader.isReady() ? "ready" : "unavailable");
  Serial.println("ota=managed-by-OtaApManager");
}

void onRfidTagDetected(const uint8_t*, uint8_t) {
  if (!registry.updateValue(ProfileConfig::SENSOR_ID_RFID_EVENT, 1.0f,
                            SensorProtocol::QUALITY_VALID)) {
    Serial.println("RFID: registry update failed");
  }
  profileManager.buzzerPulse();
}

bool registerRfidDescriptor() {
  SensorProtocol::SensorDescriptor descriptor{};
  descriptor.id = ProfileConfig::SENSOR_ID_RFID_EVENT;
  descriptor.type = static_cast<uint8_t>(SensorProtocol::SensorType::GENERIC);
  std::strncpy(descriptor.name, "rfid_tag", sizeof(descriptor.name) - 1);
  std::strncpy(descriptor.unit, "event", sizeof(descriptor.unit) - 1);
  descriptor.datatype =
      static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
  descriptor.scale = 1.0f;
  descriptor.offset = 0.0f;
  descriptor.min = 0.0f;
  descriptor.max = 1.0f;
  descriptor.periodMs = ProfileConfig::RFID_POLL_INTERVAL_MS;
  descriptor.flags = SensorProtocol::FLAG_ENABLED |
                    SensorProtocol::FLAG_EVENT_DRIVEN |
                    SensorProtocol::FLAG_READ_ONLY;
  return registry.registerSensor(descriptor);
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "help") { printHelp(); return; }
  if (line == "show") { showProvisioning(); return; }
  if (line == "save") { saveProvisioning(); return; }
  if (line == "ota save") {
    if (!validOtaPassword(otaPassword)) {
      Serial.println("ERROR: provision an OTA password (12..64 printable ASCII characters) first");
      return;
    }
    const bool saved = saveOtaProvisioning(otaPassword);
    Serial.println(saved ? "OK: OTA provisioning saved; rebooting" :
                           "ERROR: OTA provisioning save failed");
    if (saved) {
      delay(100);
      ESP.restart();
    }
    return;
  }
  if (line.startsWith("ota password ")) {
    otaPassword = line.substring(13);
    if (!validOtaPassword(otaPassword)) {
      Serial.println("ERROR: OTA password must be 12..64 printable ASCII characters");
      return;
    }
    Serial.println("OK: OTA password staged");
    return;
  }
  if (line.startsWith("name ")) {
    const String value = line.substring(5);
    if (!validNodeNameLength(value, SensorProtocol::MAX_NODE_NAME_BYTES - 1)) {
      Serial.println("ERROR: invalid name length");
      return;
    }
    nodeName = value;
    Serial.println("OK: name staged");
    return;
  }
  Serial.println("ERROR: unknown command; use help");
}
}  // namespace

void setup() {
  Serial.begin(SensorNodeConfig::SERIAL_BAUD);
  delay(200);
  Serial.println("FieldRadio ESP32-C3 BLE Sensor Node");
  loadProvisioning();
  profileManager.begin();
  profileManager.setLongPressCallback(onLongPressToggleAp);

  const bool rosterOk =
      profileSensors.begin(profileManager.activeProfile(), driverRegistry, registry);
  if (!rosterOk) {
    Serial.println("WARN: active profile roster is incomplete");
  }

  if (!registerRfidDescriptor()) {
    Serial.println("ERROR: RFID descriptor registration failed");
  }
  rfidReader.setTagCallback(onRfidTagDetected);
  if (!rfidReader.begin()) {
    Serial.println("WARN: RFID unavailable; continuing without RFID events");
  }

  otaApManager.setProfileChangeCallback(onWebProfileChange);
  otaApManager.begin();
  showProvisioning();
  printHelp();
  if (!bleServer.begin(nodeName, registry)) {
    Serial.println("FATAL: BLE server initialization failed");
    while (true) delay(1000);
  }
}

void loop() {
  static uint32_t lastSampleMs = 0;
  const uint32_t now = millis();
  if (now - lastSampleMs >= SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS) {
    lastSampleMs = now;
    (void)driverRegistry.sample(registry, now);
  }
  profileManager.task();
  otaApManager.task();
  bleServer.task();
  rfidReader.task();

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleCommand(line);
  }
  delay(5);
}
