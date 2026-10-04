#include "SensorDriverRegistry.h"
#include <Preferences.h>
#include "Config.h"
#include "ProfileConfig.h"
#include <Adafruit_BME280.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <cmath>
#if __has_include(<freertos/FreeRTOS.h>)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif
#include <cfloat>
#include <cstring>
#include <algorithm>

namespace {
uint16_t registerAddress(const DriverConfig& config) {
  return static_cast<uint16_t>(config.registerAddr);
}

class Bme280Driver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    ready_ = sensor_.begin(config.i2cAddr, &Wire);
    return ready_;
  }

  SensorProtocol::SensorType type() const override {
    switch (registerAddress(config_)) {
      case 1: return SensorProtocol::SensorType::HUMIDITY;
      case 2: return SensorProtocol::SensorType::PRESSURE;
      default: return SensorProtocol::SensorType::TEMPERATURE;
    }
  }

  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    if (!ready_) ready_ = sensor_.begin(config_.i2cAddr, &Wire);
    if (!ready_) { value = 0.0f; quality = SensorProtocol::QUALITY_STALE; return false; }
    switch (registerAddress(config_)) {
      case 1: value = sensor_.readHumidity(); break;
      case 2: value = sensor_.readPressure() / 100.0f; break;
      default: value = sensor_.readTemperature(); break;
    }
    quality = std::isfinite(value) ? SensorProtocol::QUALITY_VALID
                                    : SensorProtocol::QUALITY_STALE;
    return std::isfinite(value);
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    const char* name = "temperature";
    const char* unit = "degC";
    float minValue = -40.0f;
    float maxValue = 85.0f;
    switch (registerAddress(config_)) {
      case 1: name = "humidity"; unit = "%RH"; minValue = 0.0f; maxValue = 100.0f; break;
      case 2: name = "pressure"; unit = "hPa"; minValue = 300.0f; maxValue = 1100.0f; break;
      default: break;
    }
    std::strncpy(d.name, name, sizeof(d.name) - 1);
    std::strncpy(d.unit, unit, sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = minValue;
    d.max = maxValue;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  Adafruit_BME280 sensor_;
  bool ready_ = false;
  DriverConfig config_{};
};

class BatteryAdcDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    analogReadResolution(12);
    analogSetPinAttenuation(config_.pinSda, ADC_11db);
    return true;
  }

  SensorProtocol::SensorType type() const override { return SensorProtocol::SensorType::BATTERY; }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    const uint32_t mv = analogReadMilliVolts(config_.pinSda);
    value = (static_cast<float>(mv) / 1000.0f) * SensorNodeConfig::BATTERY_DIVIDER_RATIO;
    quality = mv == 0 ? SensorProtocol::QUALITY_STALE : SensorProtocol::QUALITY_VALID;
    return mv != 0;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    std::strncpy(d.name, "battery", sizeof(d.name) - 1);
    std::strncpy(d.unit, "V", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = 0.0f;
    d.max = 6.6f;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
};

class DigitalInputDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    pinMode(config_.pinSda, INPUT_PULLUP);
    return true;
  }

  SensorProtocol::SensorType type() const override { return SensorProtocol::SensorType::GENERIC; }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    value = digitalRead(config_.pinSda) == LOW ? 1.0f : 0.0f;
    quality = SensorProtocol::QUALITY_VALID;
    return true;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    std::strncpy(d.name, "digital", sizeof(d.name) - 1);
    std::strncpy(d.unit, "bool", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::BOOL);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = 0.0f;
    d.max = 1.0f;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
};

class GenericI2cDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    return true;
  }

  SensorProtocol::SensorType type() const override { return SensorProtocol::SensorType::GENERIC; }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    // Placeholder: a generic register transaction is not a validated
    // measurement protocol. Never expose raw register bytes as telemetry.
    value = 0.0f;
    quality = SensorProtocol::QUALITY_STALE;
    return false;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(SensorProtocol::SensorType::GENERIC);
    std::strncpy(d.name, "generic_i2c", sizeof(d.name) - 1);
    std::strncpy(d.unit, "raw", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = -FLT_MAX;
    d.max = FLT_MAX;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
};

// ---------------------------------------------------------------------------
// driver driver additions. Each driver is intentionally minimal: it reads one
// value and returns it with a quality flag. Calibration, temperature
// compensation, and environmental rating checks are caller responsibilities
// and are marked NOT VERIFIED in README.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Profile 4 - Desert & Profile 5 - Mine Tunnel driver additions.
// ---------------------------------------------------------------------------

// MQ-series / electrochemical gas sensor on ADC (CH4, CO, H2S).
// Returns raw voltage; caller-supplied descriptor carries scale/offset to
// convert to ppm. Calibration is NOT VERIFIED until hardware sample.
class GasAdcDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    analogReadResolution(12);
    analogSetPinAttenuation(config_.pinSda, ADC_11db);
    return true;
  }

  SensorProtocol::SensorType type() const override {
    return SensorProtocol::SensorType::GENERIC;
  }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    const uint32_t mv = analogReadMilliVolts(config_.pinSda);
    // Return volts; descriptor scale converts to ppm.
    value = static_cast<float>(mv) / 1000.0f;
    // A sensor that has not warmed up or is disconnected reports 0 mV.
    quality = (mv < 50) ? SensorProtocol::QUALITY_STALE
                        : SensorProtocol::QUALITY_VALID;
    return mv >= 50;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    // registerAddr selects the gas family:
    //   0 = CH4, 1 = CO, 2 = H2S
    const char* name = "gas_ch4";
    const char* unit = "V";
    float maxValue = 3.3f;
    switch (config_.registerAddr) {
      case 1: name = "gas_co";  break;
      case 2: name = "gas_h2s"; break;
      default: name = "gas_ch4"; break;
    }
    std::strncpy(d.name, name, sizeof(d.name) - 1);
    std::strncpy(d.unit, unit, sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = 0.0f;
    d.max = maxValue;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
};

// Seismic / tilt sensor over SPI (ADXL355-class) routed through GPIO3 mux.
// Placeholder: register-level protocol NOT VERIFIED.
class SeismicSpiDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    ready_ = false;  // Requires hardware-specific register sequence.
    return true;
  }

  SensorProtocol::SensorType type() const override {
    return SensorProtocol::SensorType::ACCELEROMETER;
  }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    // Placeholder: never fabricate a seismic reading.
    value = 0.0f;
    quality = SensorProtocol::QUALITY_STALE;
    return false;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    std::strncpy(d.name, "seismic_tilt", sizeof(d.name) - 1);
    std::strncpy(d.unit, "g", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = -2.0f;
    d.max = 2.0f;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
  bool ready_ = false;
};

// Dust / visibility sensor over UART (e.g. Sharp GP2Y1010 or PMS-class).
// Placeholder: UART framing NOT VERIFIED.
class DustVisibilityDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    return config_.periodMs > 0;
  }

  SensorProtocol::SensorType type() const override {
    return SensorProtocol::SensorType::GENERIC;
  }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    value = 0.0f;
    quality = SensorProtocol::QUALITY_STALE;
    return false;  // UART parser NOT VERIFIED.
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    std::strncpy(d.name, "dust_visibility", sizeof(d.name) - 1);
    std::strncpy(d.unit, "ug/m3", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = 0.0f;
    d.max = 1000.0f;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
};

class GenericAdcDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    analogReadResolution(12);
    analogSetPinAttenuation(config_.pinSda, ADC_11db);
    return true;
  }

  SensorProtocol::SensorType type() const override {
    return SensorProtocol::SensorType::GENERIC;
  }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    const uint32_t mv = analogReadMilliVolts(config_.pinSda);
    // Caller supplied scale/offset via descriptor; the driver returns volts.
    // A zero reading is treated as stale (disconnected or floating pin).
    value = static_cast<float>(mv) / 1000.0f;
    quality = mv == 0 ? SensorProtocol::QUALITY_STALE
                      : SensorProtocol::QUALITY_VALID;
    return mv != 0;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    std::strncpy(d.name, "generic_adc", sizeof(d.name) - 1);
    std::strncpy(d.unit, "V", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = 0.0f;
    d.max = 3.3f;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
};

class GenericUartDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    // UART instances are owned by the profile, not this driver. The driver
    // only validates the configuration and delegates I/O to a caller-supplied
    // SerialX. driver does not open any UART here to avoid pin conflicts.
    return config_.periodMs > 0;
  }

  SensorProtocol::SensorType type() const override {
    return SensorProtocol::SensorType::GENERIC;
  }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    // Placeholder: the profile-specific read must be implemented by the
    // caller that owns the UART peripheral. Return stale rather than fabricate
    // a value. driver intentionally does NOT invent UART protocols.
    value = 0.0f;
    quality = SensorProtocol::QUALITY_STALE;
    return false;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    std::strncpy(d.name, "generic_uart", sizeof(d.name) - 1);
    std::strncpy(d.unit, "raw", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = -FLT_MAX;
    d.max = FLT_MAX;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  DriverConfig config_{};
};

class AtlasEzoDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    ready_ = probe();
    return ready_;
  }

  SensorProtocol::SensorType type() const override {
    return SensorProtocol::SensorType::GENERIC;
  }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    if (!ready_) {
      value = 0.0f;
      quality = SensorProtocol::QUALITY_STALE;
      return false;
    }
    // Atlas EZO I2C command protocol: send 'R' to request a reading, wait for
    // the device to process, then read the response. driver sends the request
    // only; parsing the response is deferred until a hardware sample is
    // available. This avoids fabricating a protocol parser against an
    // unverified device.
    Wire.beginTransmission(config_.i2cAddr);
    Wire.write('R');
    const uint8_t txResult = Wire.endTransmission();
    if (txResult != 0) {
      value = 0.0f;
      quality = SensorProtocol::QUALITY_STALE;
      ready_ = false;
      return false;
    }
    value = 0.0f;
    quality = SensorProtocol::QUALITY_STALE;
    return false;  // Response parser NOT VERIFIED until hardware is present.
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    // config_.registerAddr selects the Atlas device family:
    //   0 = EC (salinity/conductivity), 1 = pH
    const bool isPh = config_.registerAddr == 1;
    std::strncpy(d.name, isPh ? "water_ph" : "water_ec", sizeof(d.name) - 1);
    std::strncpy(d.unit, isPh ? "pH" : "uS/cm", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = isPh ? 0.0f : 0.0f;
    d.max = isPh ? 14.0f : 100000.0f;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    return d;
  }

private:
  bool probe() {
    Wire.beginTransmission(config_.i2cAddr);
    return Wire.endTransmission() == 0;
  }

  DriverConfig config_{};
  bool ready_ = false;
};

struct OneWireBusEntry {
  int pin = -1;
  OneWire* bus = nullptr;
  DallasTemperature* sensors = nullptr;
  size_t refCount = 0;
};
constexpr size_t ONEWIRE_POOL_MAX = 4;
OneWireBusEntry gOneWirePool[ONEWIRE_POOL_MAX];

OneWireBusEntry* acquireOneWireBus(int pin) {
  for (auto& e : gOneWirePool) if (e.bus && e.pin == pin) { ++e.refCount; return &e; }
  for (auto& e : gOneWirePool) if (!e.bus) {
    e.pin = pin; e.bus = new OneWire(pin); e.sensors = new DallasTemperature(e.bus);
    e.sensors->begin(); e.refCount = 1; return &e;
  }
  return nullptr;
}

void releaseOneWireBus(int pin) {
  for (auto& e : gOneWirePool) if (e.bus && e.pin == pin) {
    if (e.refCount) --e.refCount;
    if (!e.refCount) { delete e.sensors; delete e.bus; e = OneWireBusEntry{}; }
    return;
  }
}

class OneWireTempDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    OneWireBusEntry* entry = acquireOneWireBus(config_.pinSda);
    if (!entry) return false;
    bus_ = entry->bus;
    sensors_ = entry->sensors;
    if (!sensors_ || sensors_->getDeviceCount() <= config_.channel) {
      releaseOneWireBus(config_.pinSda);
      bus_ = nullptr;
      sensors_ = nullptr;
      return false;
    }

    loadRom();
    activeIndex_ = config_.channel;
    if (romBound_ && !resolveBoundRom()) degraded_ = true;
    return sensors_ != nullptr;
  }

  SensorProtocol::SensorType type() const override {
    return SensorProtocol::SensorType::TEMPERATURE;
  }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    if (!sensors_) {
      value = 0.0f;
      quality = SensorProtocol::QUALITY_STALE;
      return false;
    }
    sensors_->requestTemperatures();
    if (romBound_ && degraded_) {
      value = 0.0f;
      quality = SensorProtocol::QUALITY_STALE;
      return false;
    }

    const float t = sensors_->getTempCByIndex(activeIndex_);
    if (t == DEVICE_DISCONNECTED_C || !std::isfinite(t)) {
      value = 0.0f;
      quality = SensorProtocol::QUALITY_STALE;
      return false;
    }

    // The first successful channel read binds the physical ROM. Future boots
    // resolve this ROM rather than relying on OneWire enumeration order.
    if (!romBound_) {
      uint8_t detected[8] = {};
      if (sensors_->getAddress(detected, activeIndex_)) {
        std::memcpy(rom_, detected, sizeof(rom_));
        if (saveRom()) romBound_ = true;
      }
    }

    value = t;
    quality = SensorProtocol::QUALITY_VALID;
    return true;
  }

  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{};
    d.id = config_.sensorId;
    d.type = static_cast<uint8_t>(type());
    const bool isSoil = config_.registerAddr == 1;
    std::strncpy(d.name, isSoil ? "soil_temp" : "water_temp",
                 sizeof(d.name) - 1);
    std::strncpy(d.unit, "degC", sizeof(d.unit) - 1);
    d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
    d.scale = 1.0f;
    d.offset = 0.0f;
    d.min = isSoil ? -20.0f : -10.0f;
    d.max = isSoil ? 60.0f : 50.0f;
    d.periodMs = config_.periodMs;
    d.flags = SensorProtocol::FLAG_ENABLED;
    if (degraded_) d.flags |= SensorProtocol::FLAG_DEGRADED;
    return d;
  }

  bool bindIndex(uint8_t index) {
    if (!sensors_ || index >= sensors_->getDeviceCount()) return false;
    uint8_t detected[8] = {};
    if (!sensors_->getAddress(detected, index)) return false;
    std::memcpy(rom_, detected, sizeof(rom_));
    if (!saveRom()) return false;
    activeIndex_ = index;
    romBound_ = true;
    degraded_ = false;
    return true;
  }

  bool currentRom(uint8_t out[8]) const {
    if (!out || !sensors_) return false;
    if (romBound_) {
      std::memcpy(out, rom_, sizeof(rom_));
      return true;
    }
    return sensors_->getAddress(out, config_.channel);
  }

  ~OneWireTempDriver() override {
    if (bus_) releaseOneWireBus(config_.pinSda);
  }

private:
  bool saveRom() const {
    Preferences prefs;
    if (!prefs.begin("sensor", false)) return false;
    char key[16] = {};
    std::snprintf(key, sizeof(key), "ow_rom_%04X",
                  static_cast<unsigned>(config_.sensorId));
    const bool ok = prefs.putBytes(key, rom_, sizeof(rom_)) == sizeof(rom_);
    prefs.end();
    return ok;
  }

  void loadRom() {
    romBound_ = false;
    degraded_ = false;
    std::memset(rom_, 0, sizeof(rom_));
    Preferences prefs;
    if (!prefs.begin("sensor", true)) return;
    char key[16] = {};
    std::snprintf(key, sizeof(key), "ow_rom_%04X",
                  static_cast<unsigned>(config_.sensorId));
    if (prefs.getBytes(key, rom_, sizeof(rom_)) == sizeof(rom_)) {
      for (uint8_t byte : rom_) {
        if (byte != 0) {
          romBound_ = true;
          break;
        }
      }
    }
    prefs.end();
  }

  bool resolveBoundRom() {
    if (!romBound_ || !sensors_) return false;
    const uint8_t count = sensors_->getDeviceCount();
    for (uint8_t i = 0; i < count; ++i) {
      uint8_t detected[8] = {};
      if (!sensors_->getAddress(detected, i)) continue;
      if (std::memcmp(detected, rom_, sizeof(rom_)) == 0) {
        activeIndex_ = i;
        return true;
      }
    }
    return false;
  }

  OneWire* bus_ = nullptr;
  DallasTemperature* sensors_ = nullptr;
  DriverConfig config_{};
  uint8_t rom_[8] = {};
  uint8_t activeIndex_ = 0;
  bool romBound_ = false;
  bool degraded_ = false;
};

struct PulseSlot {
  volatile uint32_t count = 0;
  volatile uint32_t lastEdgeUs = 0;
  uint32_t debounceUs = 2000;
  bool attached = false;
  int pin = -1;
};
PulseSlot gPulseSlots[SensorDriverRegistry::MAX_DRIVERS];

void IRAM_ATTR pulseIsrHandler(void* arg) {
  auto* slot = static_cast<PulseSlot*>(arg);
  if (!slot) return;
  const uint32_t nowUs = micros();
  if (slot->lastEdgeUs && static_cast<uint32_t>(nowUs-slot->lastEdgeUs) < slot->debounceUs) return;
  slot->lastEdgeUs = nowUs;
  ++slot->count;
}

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
template <size_t Index>
void IRAM_ATTR pulseIsrLegacySlot() {
  auto& slot = gPulseSlots[Index];
  if (!slot.attached) return;
  const uint32_t nowUs = micros();
  if (slot.lastEdgeUs &&
      static_cast<uint32_t>(nowUs - slot.lastEdgeUs) < slot.debounceUs) {
    return;
  }
  slot.lastEdgeUs = nowUs;
  ++slot.count;
}

using LegacyPulseIsr = void (*)();
constexpr LegacyPulseIsr kLegacyPulseIsrs[SensorDriverRegistry::MAX_DRIVERS] = {
    pulseIsrLegacySlot<0>, pulseIsrLegacySlot<1>, pulseIsrLegacySlot<2>,
    pulseIsrLegacySlot<3>, pulseIsrLegacySlot<4>, pulseIsrLegacySlot<5>,
    pulseIsrLegacySlot<6>, pulseIsrLegacySlot<7>, pulseIsrLegacySlot<8>,
    pulseIsrLegacySlot<9>, pulseIsrLegacySlot<10>, pulseIsrLegacySlot<11>,
    pulseIsrLegacySlot<12>,
};
#endif

class PulseCounterDriver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    if (config_.pinSda < 0 || config_.pinSda > 21 || config_.pinSda == 2) return false;
    for (auto& s : gPulseSlots) if (!s.attached) { slot_=&s; break; }
    if (!slot_) return false;
    slot_->pin=config_.pinSda; slot_->count=0; slot_->lastEdgeUs=0; slot_->attached=true;
    pinMode(slot_->pin, INPUT_PULLUP);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    attachInterruptArg(digitalPinToInterrupt(slot_->pin), pulseIsrHandler, slot_, FALLING);
#else
    // Arduino-ESP32 2.x has no attachInterruptArg(). Use a fixed ISR table
    // so each interrupt remains bound to its own PulseSlot.
    const size_t slotIndex = static_cast<size_t>(slot_ - gPulseSlots);
    if (slotIndex >= SensorDriverRegistry::MAX_DRIVERS) {
      slot_->attached = false;
      slot_->pin = -1;
      slot_ = nullptr;
      return false;
    }
    attachInterrupt(digitalPinToInterrupt(slot_->pin),
                    kLegacyPulseIsrs[slotIndex], FALLING);
#endif
    return true;
  }
  ~PulseCounterDriver() override {
    if (slot_ && slot_->attached) { detachInterrupt(digitalPinToInterrupt(slot_->pin)); slot_->attached=false; slot_->pin=-1; }
  }
  SensorProtocol::SensorType type() const override { return SensorProtocol::SensorType::GENERIC; }
  uint16_t sensorId() const override { return config_.sensorId; }
  bool read(float& value, uint8_t& quality) override {
    if (!slot_) { value=0; quality=SensorProtocol::QUALITY_STALE; return false; }
    noInterrupts(); const uint32_t pulses=slot_->count; slot_->count=0; interrupts();
    const float perPulse=config_.dataWidth?static_cast<float>(config_.dataWidth):1.0f;
    value=pulses*perPulse; quality=SensorProtocol::QUALITY_VALID; return true;
  }
  SensorProtocol::SensorDescriptor descriptor() const override {
    SensorProtocol::SensorDescriptor d{}; d.id=config_.sensorId; d.type=(uint8_t)type();
    const bool wind=config_.registerAddr==1; std::strncpy(d.name,wind?"wind_run":"rain",sizeof(d.name)-1);
    std::strncpy(d.unit,wind?"m":"mm",sizeof(d.unit)-1); d.datatype=(uint8_t)SensorProtocol::SensorDataType::FLOAT32;
    d.scale=1; d.offset=0; d.min=0; d.max=wind?10000:500; d.periodMs=config_.periodMs; d.flags=SensorProtocol::FLAG_ENABLED; return d;
  }
private: DriverConfig config_{}; PulseSlot* slot_=nullptr;
};

}  // namespace

bool SensorDriverRegistry::validConfig(const DriverConfig& config) {
  if (config.sensorId == 0 || config.periodMs == 0 || config.periodMs > 86400000UL) return false;
  if (config.driverType < DRIVER_BME280 || config.driverType > DRIVER_DUST_VISIBILITY) return false;
  if (config.pinSda > 21 || config.pinScl > 21) return false;
  if (config.driverType == DRIVER_BME280) {
    if (config.i2cAddr < 0x08 || config.i2cAddr > 0x77 || config.registerAddr > 2) return false;
  } else if (config.driverType == DRIVER_BATTERY_ADC) {
    if (config.pinSda > 4 || config.pinSda == 2) return false;
  } else if (config.driverType == DRIVER_DIGITAL_INPUT) {
    if (config.pinSda == 2 || config.pinSda == 8 || config.pinSda == 9) return false;
  } else if (config.driverType == DRIVER_GENERIC_I2C) {
    if (config.i2cAddr < 0x08 || config.i2cAddr > 0x77 ||
        (config.dataWidth != 1 && config.dataWidth != 2 && config.dataWidth != 4))
      return false;
  } else if (config.driverType == DRIVER_GENERIC_ADC) {
    // Profile time-share contract uses GPIO3 for the wind-vane ADC path.
    // Hardware capability remains a board-level validation item.
    if ((config.pinSda > 4 && config.pinSda != 3) || config.pinSda == 2) return false;
  } else if (config.driverType == DRIVER_GENERIC_UART) {
    // UART pins are not validated here; the profile owns the SerialX mapping.
    if (config.dataWidth == 0) return false;
  } else if (config.driverType == DRIVER_ATLAS_EZO) {
    if (config.i2cAddr < 0x08 || config.i2cAddr > 0x77) return false;
    if (config.registerAddr > 1) return false;  // 0=EC, 1=pH
  } else if (config.driverType == DRIVER_ONEWIRE_TEMP) {
    if (config.pinSda == 2 || config.pinSda > 21) return false;
    if (config.channel > 7) return false;
  } else if (config.driverType == DRIVER_PULSE_COUNTER) {
    if (config.pinSda == 2 || config.pinSda > 21) return false;
    if (config.registerAddr > 1 || config.dataWidth > 100) return false;
  } else if (config.driverType == DRIVER_GAS_ADC) {
    if ((config.pinSda > 4 && config.pinSda != 11) || config.pinSda == 2) return false;
    if (config.registerAddr > 2) return false;  // 0=CH4, 1=CO, 2=H2S
  } else if (config.driverType == DRIVER_SEISMIC_SPI) {
    // CS routed through GPIO3 mux; no additional pin constraints here.
  } else if (config.driverType == DRIVER_DUST_VISIBILITY) {
    if (config.dataWidth == 0) return false;
  }
  return true;
}

SensorDriver* SensorDriverRegistry::createDriver(const DriverConfig& config) {
  switch (config.driverType) {
    case DRIVER_BME280: return new Bme280Driver();
    case DRIVER_BATTERY_ADC: return new BatteryAdcDriver();
    case DRIVER_DIGITAL_INPUT: return new DigitalInputDriver();
    case DRIVER_GENERIC_I2C: return new GenericI2cDriver();
    case DRIVER_GENERIC_ADC: return new GenericAdcDriver();
    case DRIVER_GENERIC_UART: return new GenericUartDriver();
    case DRIVER_ATLAS_EZO: return new AtlasEzoDriver();
    case DRIVER_ONEWIRE_TEMP: return new OneWireTempDriver();
    case DRIVER_PULSE_COUNTER: return new PulseCounterDriver();
    case DRIVER_GAS_ADC: return new GasAdcDriver();
    case DRIVER_SEISMIC_SPI: return new SeismicSpiDriver();
    case DRIVER_DUST_VISIBILITY: return new DustVisibilityDriver();
    default: return nullptr;
  }
}

namespace {

void configureGpio3MuxPins() {
  static bool configured = false;
  if (configured) return;
  pinMode(ProfileConfig::GPIO3_MUX_S0_PIN, OUTPUT);
  pinMode(ProfileConfig::GPIO3_MUX_S1_PIN, OUTPUT);
  configured = true;
}

bool selectGpio3Mux(ProfileConfig::Gpio3MuxChannel channel) {
  configureGpio3MuxPins();
  const uint8_t value = static_cast<uint8_t>(channel);
  digitalWrite(ProfileConfig::GPIO3_MUX_S0_PIN, value & 0x01);
  digitalWrite(ProfileConfig::GPIO3_MUX_S1_PIN, (value >> 1) & 0x01);
  delayMicroseconds(ProfileConfig::GPIO3_MUX_SETTLE_US);
  return true;
}

bool selectGpio3MuxForConfig(const DriverConfig& config) {
  if (config.pinSda != ProfileConfig::GPIO3_MUX_COM_PIN) return true;
  const auto kind =
      static_cast<ProfileConfig::InterfaceKind>(config.interfaceType);
  switch (kind) {
    case ProfileConfig::InterfaceKind::OneWire:
      return selectGpio3Mux(ProfileConfig::Gpio3MuxChannel::OneWire);
    case ProfileConfig::InterfaceKind::Adc:
      return selectGpio3Mux(ProfileConfig::Gpio3MuxChannel::Adc);
    case ProfileConfig::InterfaceKind::SPI:
      return selectGpio3Mux(ProfileConfig::Gpio3MuxChannel::Adxl355Cs);
    default:
      return true;
  }
}

}  // namespace

namespace {
constexpr uint32_t CALIBRATION_MAGIC = 0x43414C31UL; // "CAL1"
constexpr uint8_t CALIBRATION_VERSION_V1 = 1;
constexpr uint8_t CALIBRATION_VERSION_V2 = 2;

struct CalibrationBlobV1 {
  uint32_t magic = CALIBRATION_MAGIC;
  uint8_t version = CALIBRATION_VERSION_V1;
  uint16_t sensorId = 0;
  uint8_t reserved = 0;
  float scale = 1.0f;
  float offset = 0.0f;
  uint32_t crc = 0;
} __attribute__((packed));

struct CalibrationBlobV2 {
  uint32_t magic = CALIBRATION_MAGIC;
  uint8_t version = CALIBRATION_VERSION_V2;
  uint16_t sensorId = 0;
  uint8_t model = 0;
  float scale = 1.0f;
  float offset = 0.0f;
  float temperatureCoeff = 0.0f;
  float nonlinearityA = 0.0f;
  float nonlinearityB = 0.0f;
  uint32_t crc = 0;
} __attribute__((packed));

template <typename T>
uint32_t calibrationCrc(const T& blob) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&blob);
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < offsetof(T, crc); ++i) {
    crc ^= p[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
  }
  return ~crc;
}

void calibrationKey(uint16_t sensorId, const char* prefix, char key[16]) {
  std::snprintf(key, 16, "%s%04X", prefix, static_cast<unsigned>(sensorId));
}
}

bool SensorDriverRegistry::loadCalibration(uint16_t sensorId, float& scale,
                                            float& offset, float& temperatureCoeff,
                                            float& nonlinearityA, float& nonlinearityB,
                                            uint8_t& model, bool& invalid) {
  scale = 1.0f; offset = 0.0f; temperatureCoeff = 0.0f;
  nonlinearityA = 0.0f; nonlinearityB = 0.0f; model = 0; invalid = false;
  Preferences prefs;
  if (!prefs.begin("calib", true)) return false;

  char v2Key[16] = {};
  calibrationKey(sensorId, "calib.v2.", v2Key);
  CalibrationBlobV2 v2{};
  const bool hasV2 = prefs.isKey(v2Key);
  if (hasV2 && prefs.getBytes(v2Key, &v2, sizeof(v2)) == sizeof(v2)) {
    const bool valid = v2.magic == CALIBRATION_MAGIC && v2.version == CALIBRATION_VERSION_V2 &&
                       v2.sensorId == sensorId && v2.model <= 2 &&
                       std::isfinite(v2.scale) && std::isfinite(v2.offset) &&
                       std::isfinite(v2.temperatureCoeff) &&
                       std::isfinite(v2.nonlinearityA) && std::isfinite(v2.nonlinearityB) &&
                       v2.scale != 0.0f && v2.crc == calibrationCrc(v2);
    if (valid) {
      scale = v2.scale; offset = v2.offset; temperatureCoeff = v2.temperatureCoeff;
      nonlinearityA = v2.nonlinearityA; nonlinearityB = v2.nonlinearityB;
      model = v2.model; prefs.end(); return true;
    }
    invalid = true;
  }

  char v1Key[16] = {};
  calibrationKey(sensorId, "calib.v1.", v1Key);
  CalibrationBlobV1 v1{};
  if (prefs.isKey(v1Key) && prefs.getBytes(v1Key, &v1, sizeof(v1)) == sizeof(v1) &&
      v1.magic == CALIBRATION_MAGIC && v1.version == CALIBRATION_VERSION_V1 &&
      v1.sensorId == sensorId && std::isfinite(v1.scale) &&
      std::isfinite(v1.offset) && v1.scale != 0.0f &&
      v1.crc == calibrationCrc(v1)) {
    scale = v1.scale; offset = v1.offset;
    CalibrationBlobV2 migrated{};
    migrated.sensorId = sensorId; migrated.scale = scale; migrated.offset = offset;
    migrated.crc = calibrationCrc(migrated);
    prefs.end();
    Preferences writePrefs;
    if (writePrefs.begin("calib", false)) {
      (void)writePrefs.putBytes(v2Key, &migrated, sizeof(migrated));
      writePrefs.end();
    }
    return true;
  }

  char scaleKey[16] = {}, offsetKey[16] = {};
  std::snprintf(scaleKey, sizeof(scaleKey), "s%04X", static_cast<unsigned>(sensorId));
  std::snprintf(offsetKey, sizeof(offsetKey), "o%04X", static_cast<unsigned>(sensorId));
  const bool hasScale = prefs.isKey(scaleKey), hasOffset = prefs.isKey(offsetKey);
  if (hasScale) scale = prefs.getFloat(scaleKey, 1.0f);
  if (hasOffset) offset = prefs.getFloat(offsetKey, 0.0f);
  const bool legacyValid = (hasScale || hasOffset) && std::isfinite(scale) &&
                           std::isfinite(offset) && scale != 0.0f;
  prefs.end();
  if (!legacyValid) { scale = 1.0f; offset = 0.0f; return false; }
  CalibrationBlobV2 migrated{};
  migrated.sensorId = sensorId; migrated.scale = scale; migrated.offset = offset;
  migrated.crc = calibrationCrc(migrated);
  Preferences writePrefs;
  if (writePrefs.begin("calib", false)) {
    (void)writePrefs.putBytes(v2Key, &migrated, sizeof(migrated));
    writePrefs.end();
  }
  return true;
}

namespace {
bool loadSamplingPeriod(uint16_t sensorId, uint32_t& periodMs) {
  Preferences prefs;
  if (!prefs.begin("sensor", true)) return false;
  char key[16] = {};
  std::snprintf(key, sizeof(key), "period_%04X",
                static_cast<unsigned>(sensorId));
  const bool found = prefs.isKey(key);
  if (found) periodMs = prefs.getUInt(key, 0);
  prefs.end();
  return found && periodMs > 0 && periodMs <= 86400000UL;
}

bool saveSamplingPeriod(uint16_t sensorId, uint32_t periodMs) {
  Preferences prefs;
  if (!prefs.begin("sensor", false)) return false;
  char key[16] = {};
  std::snprintf(key, sizeof(key), "period_%04X",
                static_cast<unsigned>(sensorId));
  const bool ok = prefs.putUInt(key, periodMs) == sizeof(uint32_t);
  prefs.end();
  return ok;
}
}

bool SensorDriverRegistry::allocateSourceSequenceBlock(uint16_t sensorId,
                                                     uint32_t& firstSequence) {
  firstSequence = 0;
  if (sensorId == 0) return false;
  Preferences prefs;
  if (!prefs.begin("seq", false)) return false;
  char key[12] = {};
  std::snprintf(key, sizeof(key), "seqh%04X", static_cast<unsigned>(sensorId));
  const uint32_t highWater = prefs.getUInt(key, 1);
  if (highWater == 0 || highWater > UINT32_MAX - Config::SENSOR_SOURCE_SEQUENCE_BLOCK) {
    prefs.end();
    return false;
  }
  const uint32_t nextHighWater = highWater + Config::SENSOR_SOURCE_SEQUENCE_BLOCK;
  const bool ok = prefs.putUInt(key, nextHighWater) == sizeof(uint32_t);
  prefs.end();
  if (!ok) return false;
  firstSequence = highWater;
  return true;
}

bool SensorDriverRegistry::rebuild(SensorRegistry& registry) {
  registry.clear();
  for (size_t i = 0; i < count_; ++i) {
    const uint32_t previousSourceSequence = entries_[i].sourceSequence;
    uint32_t persistedPeriod = 0;
    if (loadSamplingPeriod(entries_[i].config.sensorId, persistedPeriod))
      entries_[i].config.periodMs = persistedPeriod;
    delete entries_[i].driver;
    entries_[i].driver = createDriver(entries_[i].config);
    if (previousSourceSequence != 0) entries_[i].sourceSequence = previousSourceSequence;
    else {
      uint32_t firstSequence = 0;
      if (!allocateSourceSequenceBlock(entries_[i].config.sensorId, firstSequence) ||
          firstSequence == 0) {
        clear(registry);
        return false;
      }
      entries_[i].sourceSequence = firstSequence - 1U;
    }
    if (!entries_[i].driver || !validConfig(entries_[i].config) ||
        !selectGpio3MuxForConfig(entries_[i].config) ||
        !entries_[i].driver->begin(entries_[i].config)) {
      clear(registry);
      return false;
    }
    SensorProtocol::SensorDescriptor descriptor = entries_[i].driver->descriptor();
    descriptor.flags |= SensorProtocol::FLAG_HAS_SCHEMA_VERSION;
    descriptor.schemaVersion = 1;
    if (entries_[i].config.flags & DriverConfig::FLAG_PLACEHOLDER_DISABLED)
      descriptor.flags &= static_cast<uint16_t>(~SensorProtocol::FLAG_ENABLED);
    float scale = 1.0f;
    float offset = 0.0f;
    bool calibrationInvalid = false;
    if (loadCalibration(descriptor.id, scale, offset)) {
      descriptor.scale = scale;
      descriptor.offset = offset;
      descriptor.flags |= SensorProtocol::FLAG_CALIBRATED;
      if (calibrationInvalid) descriptor.flags |= SensorProtocol::FLAG_DEGRADED;
    } else if (calibrationInvalid) {
      descriptor.flags |= SensorProtocol::FLAG_DEGRADED;
    }
    entries_[i].calibrationDegraded = calibrationInvalid;
    entries_[i].descriptor = descriptor;
    entries_[i].lastSampleMs = 0;
    entries_[i].consecutiveReadFailures = 0;
    if (!registry.registerSensor(descriptor)) {
      clear(registry);
      return false;
    }
  }
  return true;
}

bool SensorDriverRegistry::add(const DriverConfig& config, SensorRegistry& registry) {
  if (!validConfig(config)) return false;
  DriverConfig previous[MAX_DRIVERS]{};
  const size_t previousCount = count_;
  for (size_t i = 0; i < count_; ++i) previous[i] = entries_[i].config;

  size_t index = count_;
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].config.sensorId == config.sensorId) { index = i; break; }
  }
  if (index == count_) {
    if (count_ >= MAX_DRIVERS) return false;
    ++count_;
  }
  entries_[index].config = config;
  // rebuild() owns replacement/destruction of every existing driver. Do not
  // clear this pointer here or an existing driver would be leaked before
  // rebuild() gets a chance to delete it.
  if (rebuild(registry)) return true;

  count_ = previousCount;
  for (size_t i = 0; i < count_; ++i) entries_[i].config = previous[i];
  return rebuild(registry);
}

void SensorDriverRegistry::clear(SensorRegistry& registry) {
  for (auto& entry : entries_) {
    delete entry.driver;
    entry = {};
  }
  count_ = 0;
  registry.clear();
}

namespace {

#if __has_include(<freertos/FreeRTOS.h>)
SemaphoreHandle_t gGpio3Mutex = nullptr;

bool lockGpio3() {
  if (!gGpio3Mutex) gGpio3Mutex = xSemaphoreCreateMutex();
  return gGpio3Mutex && xSemaphoreTake(gGpio3Mutex, portMAX_DELAY) == pdTRUE;
}

void unlockGpio3() {
  if (gGpio3Mutex) xSemaphoreGive(gGpio3Mutex);
}
#else
bool lockGpio3() { return true; }
void unlockGpio3() {}
#endif

class Gpio3TransactionGuard {
public:
  explicit Gpio3TransactionGuard(const DriverConfig& config)
      : active_(config.pinSda == ProfileConfig::GPIO3_MUX_COM_PIN) {
    if (!active_) return;
    if (!lockGpio3()) {
      active_ = false;
      Serial.println("ERROR: GPIO3 mutex unavailable");
      return;
    }
    locked_ = true;

    const auto kind =
        static_cast<ProfileConfig::InterfaceKind>(config.interfaceType);
    switch (kind) {
      case ProfileConfig::InterfaceKind::Adc:
        // Select the ADC branch before exposing COM to the ADC input.
        selectGpio3Mux(ProfileConfig::Gpio3MuxChannel::Adc);
        pinMode(ProfileConfig::GPIO3_MUX_COM_PIN, INPUT);
        delay(ProfileConfig::PROFILE2_ADC_SETTLE_MS);
        break;
      case ProfileConfig::InterfaceKind::OneWire:
        // Select the OneWire branch; the external 4.7 kOhm pull-up defines the bus.
        selectGpio3Mux(ProfileConfig::Gpio3MuxChannel::OneWire);
        pinMode(ProfileConfig::GPIO3_MUX_COM_PIN, INPUT);
        delay(ProfileConfig::PROFILE3_GPIO3_SETTLE_MS);
        break;
      case ProfileConfig::InterfaceKind::SPI:
        // Profile 2/3 ADXL355 CS is the only SPI function routed through GPIO3.
        selectGpio3Mux(ProfileConfig::Gpio3MuxChannel::Adxl355Cs);
        modeWasSpi_ = true;
        pinMode(ProfileConfig::GPIO3_MUX_COM_PIN, OUTPUT);
        digitalWrite(ProfileConfig::GPIO3_MUX_COM_PIN, HIGH);
        break;
      default:
        active_ = false;
        unlockGpio3();
        locked_ = false;
        break;
    }
  }

  ~Gpio3TransactionGuard() {
    if (!active_) return;
    if (modeWasSpi_) digitalWrite(ProfileConfig::GPIO3_MUX_COM_PIN, HIGH);
    pinMode(ProfileConfig::GPIO3_MUX_COM_PIN, INPUT);
    if (locked_) unlockGpio3();
  }

  Gpio3TransactionGuard(const Gpio3TransactionGuard&) = delete;
  Gpio3TransactionGuard& operator=(const Gpio3TransactionGuard&) = delete;

private:
  bool active_ = false;
  bool locked_ = false;
  bool modeWasSpi_ = false;
};

}  // namespace

bool SensorDriverRegistry::bindOneWireRom(uint16_t sensorId, uint8_t index) {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].config.sensorId != sensorId ||
        entries_[i].config.driverType != DRIVER_ONEWIRE_TEMP) continue;
    auto* driver = dynamic_cast<OneWireTempDriver*>(entries_[i].driver);
    return driver && driver->bindIndex(index);
  }
  return false;
}

size_t SensorDriverRegistry::getOneWireRomList(OneWireRomInfo* out,
                                               size_t maxEntries) const {
  if (!out || maxEntries == 0) return 0;
  size_t written = 0;
  for (size_t i = 0; i < count_ && written < maxEntries; ++i) {
    if (entries_[i].config.driverType != DRIVER_ONEWIRE_TEMP) continue;
    const auto* driver =
        dynamic_cast<const OneWireTempDriver*>(entries_[i].driver);
    if (!driver || !driver->currentRom(out[written].rom)) continue;
    out[written].sensorId = entries_[i].config.sensorId;
    ++written;
  }
  return written;
}

bool SensorDriverRegistry::sample(SensorRegistry& registry, uint32_t nowMs) {
  bool ok = true;
  for (auto& entry : entries_) {
    // FLAG_PLACEHOLDER_DISABLED is a hard telemetry gate. Do not consume a
    // source sequence or publish a value for disabled/unverified hardware.
    if (!SensorProtocol::isEnabled(entry.descriptor)) continue;
    if (!entry.driver) { ok = false; continue; }
    if (entry.lastSampleMs != 0 &&
        static_cast<uint32_t>(nowMs - entry.lastSampleMs) < entry.config.periodMs) continue;
    entry.lastSampleMs = nowMs;
    if (entry.sourceSequence == UINT32_MAX) {
      Serial.printf("SENSOR: source sequence exhausted id=0x%04X\n",
                    static_cast<unsigned>(entry.config.sensorId));
      ok = false;
      continue;
    }
    ++entry.sourceSequence;
    float value = 0.0f;
    uint8_t quality = SensorProtocol::QUALITY_STALE;
    Gpio3TransactionGuard gpio3Guard(entry.config);
    const bool readOk = entry.driver->read(value, quality);
    if (entry.calibrationDegraded) quality = SensorProtocol::QUALITY_STALE;
    if (!readOk) {
      if (entry.consecutiveReadFailures < 0xFF) ++entry.consecutiveReadFailures;
      if (entry.consecutiveReadFailures >= 3) {
        entry.descriptor.flags |= SensorProtocol::FLAG_DEGRADED;
        (void)registry.registerSensor(entry.descriptor);
      }
      Serial.printf("SENSOR: read failed id=0x%04X driver=%u quality=%u failures=%u\n",
                    static_cast<unsigned>(entry.config.sensorId),
                    static_cast<unsigned>(entry.config.driverType),
                    static_cast<unsigned>(quality),
                    static_cast<unsigned>(entry.consecutiveReadFailures));
      if (!std::isfinite(value)) value = 0.0f;
    } else {
      entry.consecutiveReadFailures = 0;
      if (!entry.calibrationDegraded)
        entry.descriptor.flags &= static_cast<uint16_t>(~SensorProtocol::FLAG_DEGRADED);
      (void)registry.registerSensor(entry.descriptor);
      const float calibrated = value * entry.calibrationScale + entry.calibrationOffset;
      switch (entry.calibrationModel) {
        case 0:
          value = calibrated;
          break;
        case 1:
          value = calibrated + entry.nonlinearityA * calibrated * calibrated +
                  entry.nonlinearityB;
          break;
        case 2:
          value = calibrated + entry.nonlinearityA * calibrated * calibrated +
                  entry.nonlinearityB * calibrated * calibrated * calibrated;
          break;
        default:
          value = calibrated;
          quality = SensorProtocol::QUALITY_STALE;
          break;
      }
    }
    if (!registry.updateValue(entry.config.sensorId, value, quality, entry.sourceSequence)) {
      Serial.printf("SENSOR: registry update failed id=0x%04X\n",
                    static_cast<unsigned>(entry.config.sensorId));
      ok = false;
    }
  }
  return ok;
}

uint32_t SensorDriverRegistry::minimumPeriodMs() const {
  uint32_t minimum = UINT32_MAX;
  for (size_t i = 0; i < count_; ++i)
    if (entries_[i].config.periodMs != 0 && entries_[i].config.periodMs < minimum) minimum = entries_[i].config.periodMs;
  return minimum == UINT32_MAX ? 0 : minimum;
}

bool SensorDriverRegistry::setSamplingPeriod(uint16_t sensorId, uint32_t periodMs,
                                              SensorRegistry& registry) {
  if (sensorId == 0 || periodMs == 0 || periodMs > 86400000UL) return false;
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].config.sensorId != sensorId) continue;
    const uint32_t oldPeriod = entries_[i].config.periodMs;
    // Persist first. A failed NVS write must not alter the runtime schedule.
    if (!saveSamplingPeriod(sensorId, periodMs)) return false;
    entries_[i].config.periodMs = periodMs;
    if (rebuild(registry)) return true;
    // Roll back both runtime and persisted state if driver rebuild fails.
    (void)saveSamplingPeriod(sensorId, oldPeriod);
    entries_[i].config.periodMs = oldPeriod;
    (void)rebuild(registry);
    return false;
  }
  return false;
}
