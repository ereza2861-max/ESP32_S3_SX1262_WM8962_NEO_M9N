#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include "Config.h"
#include "SensorRegistry.h"
#include "BleSensorServer.h"
#include "sensors_example.h"

namespace {
SensorRegistry registry;
BleSensorServer bleServer;
Preferences prefs;
String nodeName;
size_t configuredSensorCount = SensorNodeConfig::EXAMPLE_SENSOR_COUNT;

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  name <node-name>        set node name (max 24 bytes)");
  Serial.println("  sensors <0..5>          set example sensor count");
  Serial.println("  pin battery <gpio>      set ADC battery GPIO");
  Serial.println("  pin digital <gpio>      set button/reed GPIO");
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
  SensorsExample::setBatteryAdcPin(
      prefs.getInt("batpin", SensorNodeConfig::BATTERY_ADC_PIN));
  SensorsExample::setDigitalPin(
      prefs.getInt("digpin", SensorNodeConfig::DIGITAL_SENSOR_PIN));
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
  prefs.putString("name", nodeName);
  prefs.putUChar("count", static_cast<uint8_t>(configuredSensorCount));
  prefs.putInt("batpin", SensorsExample::batteryAdcPin());
  prefs.putInt("digpin", SensorsExample::digitalPin());
  prefs.end();
  Serial.println("Provisioning saved. Rebooting...");
  delay(100);
  ESP.restart();
}

void showProvisioning() {
  Serial.printf("name=%s sensors=%u battery_gpio=%d digital_gpio=%d\n",
                nodeName.c_str(), static_cast<unsigned>(configuredSensorCount),
                SensorsExample::batteryAdcPin(), SensorsExample::digitalPin());
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "help") { printHelp(); return; }
  if (line == "show") { showProvisioning(); return; }
  if (line == "save") { saveProvisioning(); return; }
  if (line.startsWith("name ")) {
    const String value = line.substring(5);
    if (value.length() == 0 || value.length() >= SensorProtocol::MAX_NODE_NAME_BYTES) {
      Serial.println("ERROR: invalid name length");
      return;
    }
    nodeName = value;
    Serial.println("OK: name staged");
    return;
  }
  if (line.startsWith("sensors ")) {
    const int value = line.substring(8).toInt();
    if (value < 0 || value > static_cast<int>(SensorNodeConfig::EXAMPLE_SENSOR_COUNT)) {
      Serial.println("ERROR: sensors must be 0..5");
      return;
    }
    configuredSensorCount = static_cast<size_t>(value);
    SensorsExample::registerSensors(registry, configuredSensorCount);
    Serial.println("OK: sensor count staged");
    return;
  }
  if (line.startsWith("pin battery ")) {
    const int pin = line.substring(12).toInt();
    if (pin < 0) { Serial.println("ERROR: invalid GPIO"); return; }
    SensorsExample::setBatteryAdcPin(pin);
    Serial.println("OK: battery GPIO staged");
    return;
  }
  if (line.startsWith("pin digital ")) {
    const int pin = line.substring(12).toInt();
    if (pin < 0) { Serial.println("ERROR: invalid GPIO"); return; }
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
  SensorsExample::registerSensors(registry, configuredSensorCount);
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
    SensorsExample::sample(registry);
  }
  bleServer.task();

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleCommand(line);
  }
  delay(5);
}
