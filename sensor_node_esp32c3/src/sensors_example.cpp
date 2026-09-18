#include "sensors_example.h"
#include "Config.h"
#include <Adafruit_BME280.h>
#include <Wire.h>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace {
Adafruit_BME280 gBme280;
bool gBmeReady = false;
int gBatteryPin = SensorNodeConfig::BATTERY_ADC_PIN;
int gDigitalPin = SensorNodeConfig::DIGITAL_SENSOR_PIN;

SensorProtocol::SensorDescriptor descriptor(uint16_t id, SensorProtocol::SensorType type,
                                             const char* name, const char* unit,
                                             float minValue, float maxValue,
                                             uint32_t periodMs,
                                             SensorProtocol::SensorDataType datatype =
                                                 SensorProtocol::SensorDataType::FLOAT32) {
  SensorProtocol::SensorDescriptor d{};
  d.id = id;
  d.type = static_cast<uint8_t>(type);
  std::strncpy(d.name, name, sizeof(d.name) - 1);
  std::strncpy(d.unit, unit, sizeof(d.unit) - 1);
  d.datatype = static_cast<uint8_t>(datatype);
  d.scale = 1.0f;
  d.offset = 0.0f;
  d.min = minValue;
  d.max = maxValue;
  d.periodMs = periodMs;
  d.flags = SensorProtocol::FLAG_ENABLED;
  return d;
}
}  // namespace

namespace SensorsExample {

bool begin(const PinConfig& pins) {
  gBatteryPin = pins.batteryAdcPin;
  gDigitalPin = pins.digitalPin;
  if (gDigitalPin >= 0) pinMode(gDigitalPin, INPUT_PULLUP);
  if (gBatteryPin >= 0) {
    analogReadResolution(12);
    analogSetPinAttenuation(gBatteryPin, ADC_11db);
  }

  Wire.begin(SensorNodeConfig::BME280_SDA_PIN, SensorNodeConfig::BME280_SCL_PIN);
  gBmeReady = gBme280.begin(SensorNodeConfig::BME280_ADDRESS, &Wire);
  if (!gBmeReady) {
    Serial.println("WARN: BME280 not detected; temperature/humidity/pressure will be stale");
  }
  return true;
}

void registerSensors(SensorRegistry& registry, size_t sensorCount) {
  registry.clear();
  const size_t count = std::min(sensorCount, SensorNodeConfig::EXAMPLE_SENSOR_COUNT);
  if (count >= 1) registry.registerSensor(descriptor(1, SensorProtocol::SensorType::TEMPERATURE,
                                                       "temperature", "degC", -40.0f, 85.0f,
                                                       SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS));
  if (count >= 2) registry.registerSensor(descriptor(2, SensorProtocol::SensorType::HUMIDITY,
                                                       "humidity", "%RH", 0.0f, 100.0f,
                                                       SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS));
  if (count >= 3) registry.registerSensor(descriptor(3, SensorProtocol::SensorType::PRESSURE,
                                                       "pressure", "hPa", 300.0f, 1100.0f,
                                                       SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS));
  if (count >= 4) registry.registerSensor(descriptor(4, SensorProtocol::SensorType::BATTERY,
                                                       "battery", "V", 0.0f, 6.6f,
                                                       SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS));
  if (count >= 5) registry.registerSensor(descriptor(5, SensorProtocol::SensorType::GENERIC,
                                                       "reed", "bool", 0.0f, 1.0f,
                                                       SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS,
                                                       SensorProtocol::SensorDataType::BOOL));
}

void sample(SensorRegistry& registry) {
  const bool haveBmeSensors = registry.find(1) >= 0 || registry.find(2) >= 0 || registry.find(3) >= 0;
  if (haveBmeSensors && gBmeReady) {
    if (registry.find(1) >= 0) {
      const float temperature = gBme280.readTemperature();
      registry.updateValue(1, temperature, std::isfinite(temperature)
                                           ? SensorProtocol::QUALITY_VALID
                                           : SensorProtocol::QUALITY_STALE);
    }
    if (registry.find(2) >= 0) {
      const float humidity = gBme280.readHumidity();
      registry.updateValue(2, humidity, std::isfinite(humidity)
                                       ? SensorProtocol::QUALITY_VALID
                                       : SensorProtocol::QUALITY_STALE);
    }
    if (registry.find(3) >= 0) {
      const float pressure = gBme280.readPressure() / 100.0f;
      registry.updateValue(3, pressure, std::isfinite(pressure)
                                       ? SensorProtocol::QUALITY_VALID
                                       : SensorProtocol::QUALITY_STALE);
    }
  } else if (haveBmeSensors) {
    if (registry.find(1) >= 0) registry.updateValue(1, 0.0f, SensorProtocol::QUALITY_STALE);
    if (registry.find(2) >= 0) registry.updateValue(2, 0.0f, SensorProtocol::QUALITY_STALE);
    if (registry.find(3) >= 0) registry.updateValue(3, 0.0f, SensorProtocol::QUALITY_STALE);
  }

  if (registry.find(4) >= 0 && gBatteryPin >= 0) {
    const uint32_t millivolts = analogReadMilliVolts(gBatteryPin);
    const float voltage = (static_cast<float>(millivolts) / 1000.0f) *
                          SensorNodeConfig::BATTERY_DIVIDER_RATIO;
    const auto quality = millivolts == 0U
                             ? SensorProtocol::QUALITY_STALE
                             : SensorProtocol::QUALITY_VALID;
    registry.updateValue(4, voltage, quality);
  }

  if (registry.find(5) >= 0 && gDigitalPin >= 0) {
    const bool active = digitalRead(gDigitalPin) == LOW;
    registry.updateValue(5, active ? 1.0f : 0.0f, SensorProtocol::QUALITY_VALID);
  }
}

void setBatteryAdcPin(int pin) {
  gBatteryPin = pin;
  if (gBatteryPin >= 0) {
    analogReadResolution(12);
    analogSetPinAttenuation(gBatteryPin, ADC_11db);
  }
}
void setDigitalPin(int pin) {
  gDigitalPin = pin;
  if (gDigitalPin >= 0) pinMode(gDigitalPin, INPUT_PULLUP);
}
int batteryAdcPin() { return gBatteryPin; }
int digitalPin() { return gDigitalPin; }

}  // namespace SensorsExample
