#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include "Config.h"
#include "SensorRegistry.h"
#include "BleSensorServer.h"
#include "sensors_example.h"
#include "SensorDriverRegistry.h"

namespace {
SensorRegistry registry;
SensorDriverRegistry driverRegistry;
BleSensorServer bleServer;
Preferences prefs;
String nodeName;
size_t configuredSensorCount = SensorNodeConfig::EXAMPLE_SENSOR_COUNT;

bool validBatteryPin(int pin) {
  // ESP32-C3 ADC1 is GPIO0..4. GPIO2 is a strapping pin and is excluded.
  return pin >= 0 && pin <= 4 && pin != 2;
}

bool validDigitalPin(int pin) {
  // Keep the interactive provisioning command away from boot straps, VDD_SPI,
  // flash pins and USB-JTAG. BME280 already owns GPIO8/9.
  switch (pin) {
    case 0:
    case 1:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 10:
    case 20:
    case 21:
      return true;
    default:
      return false;
  }
}

bool validNodeNameLength(const String& value, size_t maxBytes) {
  return value.length() > 0 && value.length() <= maxBytes;
}

bool parseStrictInt(const String& text, int& out) {
  String value = text;
  value.trim();
  if (value.isEmpty()) return false;
  size_t pos = 0;
  bool negative = false;
  if (value[0] == '+' || value[0] == '-') {
    negative = value[0] == '-';
    pos = 1;
  }
  if (pos >= value.length()) return false;
  long parsed = 0;
  for (; pos < value.length(); ++pos) {
    const char c = value[pos];
    if (c < '0' || c > '9') return false;
    const int digit = c - '0';
    if (parsed > (INT32_MAX - digit) / 10L) return false;
    parsed = parsed * 10L + digit;
  }
  if (negative) parsed = -parsed;
  out = static_cast<int>(parsed);
  return true;
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  name <node-name>        set node name (max 24 bytes)");
  Serial.println("  sensors <0..5>          set legacy example sensor count");
  Serial.println("  driver add <type> <sensorId> [args...]");
  Serial.println("    bme280 <sda> <scl> <addr> <channel 0..2> <periodMs>");
  Serial.println("    battery <pin> <periodMs>");
  Serial.println("    digital <pin> <periodMs>");
  Serial.println("    generici2c <sda> <scl> <addr> <register> <width 1|2|4> <periodMs> [flags]");
  Serial.println("    flags: bit0=signed bit1=little-endian bit2=16-bit-register");
  Serial.println("  driver remove <sensorId>");
  Serial.println("  driver list");
  Serial.println("  driver save");
  Serial.println("  pin battery <gpio>      set legacy ADC1 battery GPIO (0,1,3,4)");
  Serial.println("  pin digital <gpio>      set legacy safe digital GPIO");
  Serial.println("  show                    show current provisioning");
  Serial.println("  save                    save provisioning to NVS and reboot");
  Serial.println("  help");
}

void loadProvisioning() {
  if (!prefs.begin("sensor", false)) {
    nodeName = SensorNodeConfig::DEFAULT_NODE_NAME;
    return;
  }
  nodeName = prefs.getString("name", SensorNodeConfig::DEFAULT_NODE_NAME);
  configuredSensorCount = std::min<size_t>(
      prefs.getUChar("count", static_cast<uint8_t>(SensorNodeConfig::EXAMPLE_SENSOR_COUNT)),
      SensorNodeConfig::EXAMPLE_SENSOR_COUNT);
  const int batteryPin =
      prefs.getInt("batpin", SensorNodeConfig::BATTERY_ADC_PIN);
  const int digitalPin =
      prefs.getInt("digpin", SensorNodeConfig::DIGITAL_SENSOR_PIN);
  if (!validBatteryPin(batteryPin) || !validDigitalPin(digitalPin) ||
      batteryPin == digitalPin) {
    Serial.println("WARN: invalid persisted pin map; restoring safe defaults");
    SensorsExample::setBatteryAdcPin(SensorNodeConfig::BATTERY_ADC_PIN);
    SensorsExample::setDigitalPin(SensorNodeConfig::DIGITAL_SENSOR_PIN);
  } else {
    SensorsExample::setBatteryAdcPin(batteryPin);
    SensorsExample::setDigitalPin(digitalPin);
  }
  prefs.end();
}

void saveProvisioning() {
  if (!prefs.begin("sensor", false)) {
    Serial.println("ERROR: NVS open failed");
    return;
  }
  if (nodeName.length() > SensorProtocol::MAX_NODE_NAME_BYTES - 1) {
    Serial.println("ERROR: node name too long");
    prefs.end();
    return;
  }
  const bool ok =
      prefs.putString("name", nodeName) > 0 &&
      prefs.putUChar("count", static_cast<uint8_t>(configuredSensorCount)) == sizeof(uint8_t) &&
      prefs.putInt("batpin", SensorsExample::batteryAdcPin()) == sizeof(int32_t) &&
      prefs.putInt("digpin", SensorsExample::digitalPin()) == sizeof(int32_t);
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
  Serial.printf("name=%s sensors=%u battery_gpio=%d digital_gpio=%d drivers=%u\n",
                nodeName.c_str(), static_cast<unsigned>(configuredSensorCount),
                SensorsExample::batteryAdcPin(), SensorsExample::digitalPin(),
                static_cast<unsigned>(driverRegistry.count()));
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "help") { printHelp(); return; }
  if (line == "show") { showProvisioning(); return; }
  if (line == "save") { saveProvisioning(); return; }
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
  if (line == "driver list") {
    Serial.println(driverRegistry.listJson());
    return;
  }
  if (line == "driver save") {
    Serial.println(driverRegistry.save() ? "OK: driver configuration saved" : "ERROR: driver configuration save failed");
    return;
  }
  if (line.startsWith("driver remove ")) {
    int sensorId = 0;
    if (!parseStrictInt(line.substring(14), sensorId) || sensorId <= 0 ||
        !driverRegistry.remove(static_cast<uint16_t>(sensorId), registry)) {
      Serial.println("ERROR: driver remove failed");
      return;
    }
    Serial.println("OK: driver removed");
    return;
  }
  if (line.startsWith("driver add ")) {
    String rest = line.substring(11);
    String args[10]{};
    size_t argc = 0;
    while (rest.length() && argc < 10) {
      rest.trim();
      const int sep = rest.indexOf(' ');
      if (sep < 0) {
        args[argc++] = rest;
        rest = "";
      } else {
        args[argc++] = rest.substring(0, sep);
        rest = rest.substring(sep + 1);
      }
    }
    if (argc < 3) {
      Serial.println("ERROR: driver add syntax; use help");
      return;
    }

    DriverConfig config{};
    if (args[0] == "bme280") config.driverType = SensorDriverRegistry::DRIVER_BME280;
    else if (args[0] == "battery") config.driverType = SensorDriverRegistry::DRIVER_BATTERY_ADC;
    else if (args[0] == "digital") config.driverType = SensorDriverRegistry::DRIVER_DIGITAL_INPUT;
    else if (args[0] == "generici2c") config.driverType = SensorDriverRegistry::DRIVER_GENERIC_I2C;
    else {
      Serial.println("ERROR: unknown driver type");
      return;
    }

    int values[8] = {};
    const size_t numericStart = 1;
    for (size_t i = numericStart; i < argc && i - numericStart < 8; ++i) {
      if (!parseStrictInt(args[i], values[i - numericStart])) {
        Serial.println("ERROR: numeric driver argument invalid");
        return;
      }
    }
    int sensorId = values[0];
    if (sensorId <= 0 || sensorId > 65535) {
      Serial.println("ERROR: sensorId must be 1..65535");
      return;
    }
    config.sensorId = static_cast<uint16_t>(sensorId);

    if (config.driverType == SensorDriverRegistry::DRIVER_BME280) {
      if (argc != 7 || values[1] < 0 || values[1] > 21 ||
          values[2] < 0 || values[2] > 21 || values[3] < 0x08 || values[3] > 0x77 ||
          values[4] < 0 || values[4] > 2 || values[5] <= 0) {
        Serial.println("ERROR: bme280 requires <sda> <scl> <addr> <channel 0..2> <periodMs>");
        return;
      }
      config.pinSda = static_cast<uint8_t>(values[1]);
      config.pinScl = static_cast<uint8_t>(values[2]);
      config.i2cAddr = static_cast<uint8_t>(values[3]);
      config.registerAddr = static_cast<uint16_t>(values[4]);
      config.periodMs = static_cast<uint32_t>(values[5]);
    } else if (config.driverType == SensorDriverRegistry::DRIVER_BATTERY_ADC ||
               config.driverType == SensorDriverRegistry::DRIVER_DIGITAL_INPUT) {
      if (argc != 3 || values[1] < 0 || values[1] > 21 || values[2] <= 0) {
        Serial.println("ERROR: battery/digital requires <pin> <periodMs>");
        return;
      }
      config.pinSda = static_cast<uint8_t>(values[1]);
      config.periodMs = static_cast<uint32_t>(values[2]);
    } else {
      if (argc < 7 || argc > 8 || values[1] < 0 || values[1] > 21 ||
          values[2] < 0 || values[2] > 21 || values[3] < 0x08 || values[3] > 0x77 ||
          values[4] < 0 || values[4] > 65535 || values[5] <= 0 ||
          (values[6] != 1 && values[6] != 2 && values[6] != 4)) {
        Serial.println("ERROR: generici2c requires <sda> <scl> <addr> <register> <width 1|2|4> <periodMs> [flags]");
        return;
      }
      config.pinSda = static_cast<uint8_t>(values[1]);
      config.pinScl = static_cast<uint8_t>(values[2]);
      config.i2cAddr = static_cast<uint8_t>(values[3]);
      config.registerAddr = static_cast<uint16_t>(values[4]);
      config.dataWidth = static_cast<uint8_t>(values[5]);
      config.periodMs = static_cast<uint32_t>(values[6]);
      if (argc == 8) config.flags = static_cast<uint16_t>(values[7]);
    }

    if (!driverRegistry.add(config, registry)) {
      Serial.println("ERROR: driver add rejected");
      return;
    }
    Serial.println("OK: driver staged");
    return;
  }
  if (line.startsWith("sensors ")) {
    int value = 0;
    if (!parseStrictInt(line.substring(8), value) ||
        value < 0 || value > static_cast<int>(SensorNodeConfig::EXAMPLE_SENSOR_COUNT)) {
      Serial.println("ERROR: sensors must be 0..5");
      return;
    }
    configuredSensorCount = static_cast<size_t>(value);
    SensorsExample::registerSensors(registry, configuredSensorCount);
    Serial.println("OK: sensor count staged");
    return;
  }
  if (line.startsWith("pin battery ")) {
    int pin = 0;
    if (!parseStrictInt(line.substring(12), pin) || !validBatteryPin(pin)) {
      Serial.println("ERROR: battery GPIO must be ADC1 GPIO0,1,3,4");
      return;
    }
    if (pin == SensorsExample::digitalPin()) {
      Serial.println("ERROR: battery GPIO conflicts with digital GPIO");
      return;
    }
    SensorsExample::setBatteryAdcPin(pin);
    Serial.println("OK: battery GPIO staged");
    return;
  }
  if (line.startsWith("pin digital ")) {
    int pin = 0;
    if (!parseStrictInt(line.substring(12), pin) || !validDigitalPin(pin)) {
      Serial.println("ERROR: digital GPIO is reserved, strapping, USB-JTAG, or unavailable");
      return;
    }
    if (pin == SensorsExample::batteryAdcPin()) {
      Serial.println("ERROR: digital GPIO conflicts with battery GPIO");
      return;
    }
    SensorsExample::setDigitalPin(pin);
    Serial.println("OK: digital GPIO staged");
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
  SensorsExample::begin({SensorsExample::batteryAdcPin(), SensorsExample::digitalPin()});
  if (!driverRegistry.load(registry)) {
    SensorsExample::registerSensors(registry, configuredSensorCount);
    Serial.println("INFO: sensor_cfg empty/unavailable; using legacy example sensor fallback");
  } else {
    Serial.println("INFO: runtime sensor driver registry loaded");
  }
  showProvisioning();
  Serial.printf("drivers=%s\n", driverRegistry.listJson().c_str());
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
    if (driverRegistry.count() > 0) (void)driverRegistry.sample(registry, now);
    else SensorsExample::sample(registry);
  }
  bleServer.task();

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleCommand(line);
  }
  delay(5);
}
