#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
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
constexpr uint8_t BOOT_LOOP_THRESHOLD = 5;
bool recoveryMode = false;
bool bootLoopCleared = false;
bool bleResetRequested = false;

bool updateBootLoopCount(bool reset) {
  Preferences bootPrefs;
  if (!bootPrefs.begin("sensor", false)) return false;
  uint32_t count = bootPrefs.getUInt("boot_loop_count", 0);
  if (reset) count = 0;
  else if (count < UINT32_MAX) ++count;
  const bool ok = bootPrefs.putUInt("boot_loop_count", count) > 0;
  bootPrefs.end();
  return ok;
}

uint32_t bootLoopCount() {
  Preferences bootPrefs;
  if (!bootPrefs.begin("sensor", true)) return 0;
  const uint32_t count = bootPrefs.getUInt("boot_loop_count", 0);
  bootPrefs.end();
  return count;
}

void watchdogInit() {
  esp_task_wdt_config_t cfg{};
  cfg.timeout_ms = 10000;
  cfg.idle_core_mask = 1U << 0;
  cfg.trigger_panic = true;
  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&cfg);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    (void)esp_task_wdt_add(nullptr);
  } else {
    Serial.printf("WARN: sensor Task WDT setup failed: %s\n", esp_err_to_name(err));
  }
}

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
  Serial.println("  recovery clear         clear boot-loop recovery and reboot");
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

bool onBleCommand(const SensorProtocol::CommandRequest& request,
                  SensorProtocol::CommandResponse& response) {
  switch (request.commandId) {
    case SensorProtocol::COMMAND_SET_SAMPLING_PERIOD:
      if (request.sensorId == 0 ||
          !driverRegistry.setSamplingPeriod(request.sensorId, request.argument, registry)) {
        response.errorCode = 10;
        return false;
      }
      return true;
    case SensorProtocol::COMMAND_REQUEST_DESCRIPTOR_REFRESH:
      bleServer.refreshDescriptorCache();
      return true;
    case SensorProtocol::COMMAND_REQUEST_SENSOR_RESET:
      bleResetRequested = true;
      return true;
    default:
      response.errorCode = 11;
      return false;
  }
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
                    SensorProtocol::FLAG_READ_ONLY |
                    SensorProtocol::FLAG_HAS_SCHEMA_VERSION;
  descriptor.schemaVersion = 1;
  return registry.registerSensor(descriptor);
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "help") { printHelp(); return; }
  if (line == "show") { showProvisioning(); return; }
  if (line == "save") { saveProvisioning(); return; }
  if (line == "recovery clear") {
    (void)updateBootLoopCount(true);
    Serial.println("OK: boot-loop recovery cleared; rebooting");
    delay(100);
    ESP.restart();
    return;
  }
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
  watchdogInit();
  (void)updateBootLoopCount(false);
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
  bleServer.setCommandHandler(onBleCommand);
  showProvisioning();
  printHelp();
  const uint32_t loopCount = bootLoopCount();
  if (loopCount > BOOT_LOOP_THRESHOLD) {
    recoveryMode = true;
    Serial.printf("RECOVERY: boot_loop_count=%lu; BLE disabled, OTA AP window only\n",
                  static_cast<unsigned long>(loopCount));
    otaApManager.beginRecoveryWindow();
  }
  if (recoveryMode) return;
  if (!bleServer.begin(nodeName, registry)) {
    Serial.println("FATAL: BLE server initialization failed; entering recovery cycle");
    otaApManager.beginRecoveryWindow();
    for (;;) {
      esp_task_wdt_reset();
      otaApManager.task();
      if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));
      delay(100);
    }
  }
}

void loop() {
  static uint32_t lastSampleMs = 0;
  static bool firstSuccessfulLoop = false;
  const uint32_t minPeriodMs = driverRegistry.minimumPeriodMs();
  const uint32_t tickMs = max<uint32_t>(
      SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS, minPeriodMs);
  const uint32_t now = millis();
  bool sampleOk = true;
  if (now - lastSampleMs >= tickMs) {
    lastSampleMs = now;
    // The driver registry owns per-sensor periodMs scheduling. The loop tick
    // is never shorter than the configured base period or registered minimum.
    sampleOk = driverRegistry.sample(registry, now);
  }
  if (sampleOk && bleServer.isRunning() && !firstSuccessfulLoop) {
    firstSuccessfulLoop = true;
    // Successful BLE initialization plus one successful scheduler pass clears
    // the persistent boot-loop guard.
    if (!bootLoopCleared) {
      (void)updateBootLoopCount(true);
      bootLoopCleared = true;
    }
  }
  if (recoveryMode) {
    esp_task_wdt_reset();
    otaApManager.task();
    if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));
    delay(50);
    return;
  }
  esp_task_wdt_reset();
  profileManager.task();
  otaApManager.task();
  bleServer.task();
  rfidReader.task();
  if (bleResetRequested) {
    delay(100);
    ESP.restart();
  }

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleCommand(line);
  }
  esp_task_wdt_reset();
  delay(5);
}
