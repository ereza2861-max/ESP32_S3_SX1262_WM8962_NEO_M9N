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
constexpr uint8_t BOOT_LOOP_THRESHOLD = 3;
constexpr char WDT_FAULT_LATCH_KEY[] = "wdt_fault_latched";
bool recoveryMode = false;
bool bootLoopCleared = false;
bool bleResetRequested = false;

bool updateBootLoopCount(bool reset) {
  Preferences bootPrefs;
  if (!bootPrefs.begin("sensor", false)) return false;
  uint32_t count = bootPrefs.getUInt("boot_loop_count", 0);
  if (reset) count = 0;
  else if (count < UINT32_MAX) ++count;
  bool latchOk = true;
  if (!reset && count >= BOOT_LOOP_THRESHOLD)
    latchOk = bootPrefs.putBool(WDT_FAULT_LATCH_KEY, true);
  const bool ok = bootPrefs.putUInt("boot_loop_count", count) > 0 && latchOk;
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

bool watchdogFaultLatched() {
  Preferences prefs;
  if (!prefs.begin("sensor", true)) return false;
  const bool latched = prefs.getBool(WDT_FAULT_LATCH_KEY, false);
  prefs.end();
  return latched;
}

bool clearWatchdogFaultLatch() {
  Preferences prefs;
  if (!prefs.begin("sensor", false)) return false;
  const bool ok = prefs.putBool(WDT_FAULT_LATCH_KEY, false) &&
                  prefs.putUInt("boot_loop_count", 0) > 0;
  prefs.end();
  return ok;
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
  return otaApManager.provisionPassword(otaPass);
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
  Serial.println("  profile force <0-5>    force a non-production profile for provisioning");
  Serial.println("  profile enable <0-5>   enable a validated profile roster");
  Serial.println("  profile disable <0-5>  disable an unvalidated profile roster");
  Serial.println("  recovery clear         clear boot-loop recovery and reboot");
  Serial.println("  ow bind <sensorId> <index>  bind DS18B20 ROM to a channel");
  Serial.println("  ow list                  list detected/bound DS18B20 ROMs");
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
  const uint8_t profileIndex = static_cast<uint8_t>(profileManager.activeProfile());
  Serial.printf("profile_state=%u incomplete=%s production_ready=%s\\n",
                static_cast<unsigned>(profileManager.currentState()),
                profileSensors.profileIncomplete() ? "yes" : "no",
                ProfileConfig::PRODUCTION_READY[profileIndex] ? "yes" : "no");
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
    case SensorProtocol::COMMAND_BIND_ROM:
      if (request.sensorId == 0 ||
          !driverRegistry.bindOneWireRom(
              request.sensorId, static_cast<uint8_t>(request.argument))) {
        response.errorCode = 12;
        return false;
      }
      return true;
    case SensorProtocol::COMMAND_GET_ROM_LIST: {
      SensorDriverRegistry::OneWireRomInfo list[SensorDriverRegistry::MAX_DRIVERS]{};
      const size_t total = driverRegistry.getOneWireRomList(
          list, SensorDriverRegistry::MAX_DRIVERS);
      constexpr size_t ENTRIES_PER_PACKET = 8;
      const size_t packetCount = total == 0 ? 1 :
          (total + ENTRIES_PER_PACKET - 1) / ENTRIES_PER_PACKET;
      SensorProtocol::RomListNotification packets[2]{};
      const size_t boundedPacketCount = packetCount > 2 ? 2 : packetCount;
      for (size_t p = 0; p < boundedPacketCount; ++p) {
        packets[p].chunkIndex = static_cast<uint8_t>(p);
        packets[p].chunkCount = static_cast<uint8_t>(boundedPacketCount);
        const size_t first = p * ENTRIES_PER_PACKET;
        const size_t remaining = total > first ? total - first : 0;
        packets[p].entryCount = static_cast<uint8_t>(
            remaining > ENTRIES_PER_PACKET ? ENTRIES_PER_PACKET : remaining);
        for (size_t i = 0; i < packets[p].entryCount; ++i) {
          packets[p].entries[i].sensorId = list[first + i].sensorId;
          std::memcpy(packets[p].entries[i].rom, list[first + i].rom, 8);
        }
      }
      bleServer.notifyRomList(packets, boundedPacketCount);
      return true;
    }
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
                    SensorProtocol::FLAG_HAS_SOURCE_SEQUENCE |
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
  if (line.startsWith("profile force ")) {
    const int raw = line.substring(14).toInt();
    if (raw >= 0 && raw < ProfileConfig::PROFILE_COUNT &&
        profileManager.setProfile(static_cast<ProfileConfig::Profile>(raw), true)) {
      Serial.printf("OK: profile %d forced for provisioning; reboot required\n", raw);
    } else {
      Serial.println("ERROR: profile force failed");
    }
    return;
  }
  if (line.startsWith("profile enable ")) {
    const int raw = line.substring(15).toInt();
    if (raw >= 0 && raw < ProfileConfig::PROFILE_COUNT &&
        profileManager.setProfilePlaceholderDisabled(
            static_cast<ProfileConfig::Profile>(raw), false)) {
      Serial.printf("OK: profile %d enabled; reboot required\n", raw);
    } else {
      Serial.println("ERROR: profile enable failed");
    }
    return;
  }
  if (line.startsWith("profile disable ")) {
    const int raw = line.substring(16).toInt();
    if (raw >= 0 && raw < ProfileConfig::PROFILE_COUNT &&
        profileManager.setProfilePlaceholderDisabled(
            static_cast<ProfileConfig::Profile>(raw), true)) {
      Serial.printf("OK: profile %d disabled; reboot required\n", raw);
    } else {
      Serial.println("ERROR: profile disable failed");
    }
    return;
  }
  if (line == "recovery clear") {
    if (!clearWatchdogFaultLatch()) {
      Serial.println("ERROR: recovery latch clear failed");
      return;
    }
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
  if (line.startsWith("ow bind ")) {
    const int separator = line.indexOf(' ', 8);
    if (separator < 0) {
      Serial.println("ERROR: use ow bind <sensorId> <index>");
      return;
    }
    const uint16_t sensorId =
        static_cast<uint16_t>(line.substring(8, separator).toInt());
    const uint8_t index =
        static_cast<uint8_t>(line.substring(separator + 1).toInt());
    Serial.println(driverRegistry.bindOneWireRom(sensorId, index)
                       ? "OK: OneWire ROM bound"
                       : "ERROR: OneWire ROM bind failed");
    return;
  }
  if (line == "ow list") {
    SensorDriverRegistry::OneWireRomInfo list[SensorDriverRegistry::MAX_DRIVERS]{};
    const size_t count = driverRegistry.getOneWireRomList(
        list, SensorDriverRegistry::MAX_DRIVERS);
    for (size_t i = 0; i < count; ++i) {
      Serial.printf("OW 0x%04X ROM=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                    static_cast<unsigned>(list[i].sensorId),
                    list[i].rom[0], list[i].rom[1], list[i].rom[2], list[i].rom[3],
                    list[i].rom[4], list[i].rom[5], list[i].rom[6], list[i].rom[7]);
    }
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
      profileSensors.begin(profileManager.activeProfile(), driverRegistry, registry,
                           profileManager.profilePlaceholderDisabled(
                               profileManager.activeProfile()));
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
  otaApManager.setRecoveryClearCallback(clearWatchdogFaultLatch);
  otaApManager.begin();
  bleServer.setCommandHandler(onBleCommand);
  showProvisioning();
  printHelp();
  const uint32_t loopCount = bootLoopCount();
  if (loopCount >= BOOT_LOOP_THRESHOLD || watchdogFaultLatched()) {
    recoveryMode = true;
    profileManager.setState(ProfileConfig::SensorNodeState::RECOVERY);
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
  if (now - lastSampleMs >= tickMs) {
    lastSampleMs = now;
    // The driver registry owns per-sensor periodMs scheduling. The loop tick
    // is never shorter than the configured base period or registered minimum.
    profileManager.setState(ProfileConfig::SensorNodeState::MEASURING);
    const bool sampleOk = driverRegistry.sample(registry, now);
    profileManager.setState(sampleOk ? ProfileConfig::SensorNodeState::READY : ProfileConfig::SensorNodeState::DEGRADED);
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
  if (bleServer.isRunning() && !firstSuccessfulLoop) {
    firstSuccessfulLoop = true;
    // A complete healthy BLE scheduler iteration is the boot-health gate.
    // Sensor read failures do not participate in boot-loop recovery.
    if (!bootLoopCleared) {
      (void)updateBootLoopCount(true);
      bootLoopCleared = true;
    }
  }
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
