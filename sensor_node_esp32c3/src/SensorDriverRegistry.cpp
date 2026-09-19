#include "SensorDriverRegistry.h"
#include "Config.h"
#include <Adafruit_BME280.h>
#include <Preferences.h>
#include <Wire.h>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <algorithm>

namespace {
constexpr uint32_t CONFIG_MAGIC = 0x47464353UL; // "SCFG"
constexpr uint8_t CONFIG_VERSION = 1;

#pragma pack(push, 1)
struct DiskEntry {
  uint8_t driverType;
  uint16_t sensorId;
  uint8_t pinSda;
  uint8_t pinScl;
  uint8_t i2cAddr;
  uint8_t reserved[3]; // register address (LE16) + generic data width
  uint32_t periodMs;
  uint16_t flags;
};

struct DiskConfig {
  uint32_t magic = CONFIG_MAGIC;
  uint8_t version = CONFIG_VERSION;
  uint8_t count = 0;
  uint16_t reserved = 0;
  DiskEntry entries[SensorDriverRegistry::MAX_DRIVERS]{};
  uint32_t crc32 = 0;
};
#pragma pack(pop)

static_assert(sizeof(DiskEntry) == 15, "DriverEntry wire layout changed");
static_assert(sizeof(DiskConfig) == 4 + 1 + 1 + 2 + 15 * SensorDriverRegistry::MAX_DRIVERS + 4,
              "sensor config layout changed");

uint16_t registerAddress(const DriverConfig& config) {
  return static_cast<uint16_t>(config.registerAddr);
}

class Bme280Driver final : public SensorDriver {
public:
  bool begin(const DriverConfig& config) override {
    config_ = config;
    Wire.begin(config.pinSda, config.pinScl);
    ready_ = sensor_.begin(config.i2cAddr, &Wire);
    return true;
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
    Wire.begin(config_.pinSda, config_.pinScl);
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
    Wire.begin(config_.pinSda, config_.pinScl);
    return true;
  }

  SensorProtocol::SensorType type() const override { return SensorProtocol::SensorType::GENERIC; }
  uint16_t sensorId() const override { return config_.sensorId; }

  bool read(float& value, uint8_t& quality) override {
    Wire.begin(config_.pinSda, config_.pinScl);
    Wire.beginTransmission(config_.i2cAddr);
    if (config_.flags & SensorDriverRegistry::FLAG_REGISTER_16BIT)
      Wire.write(static_cast<uint8_t>(config_.registerAddr >> 8));
    Wire.write(static_cast<uint8_t>(config_.registerAddr));
    if (Wire.endTransmission(false) != 0) {
      value = 0.0f;
      quality = SensorProtocol::QUALITY_STALE;
      return false;
    }
    if (Wire.requestFrom(static_cast<int>(config_.i2cAddr),
                         static_cast<int>(config_.dataWidth), true) != config_.dataWidth) {
      value = 0.0f;
      quality = SensorProtocol::QUALITY_STALE;
      return false;
    }

    uint32_t raw = 0;
    if (config_.flags & SensorDriverRegistry::FLAG_LITTLE_ENDIAN) {
      for (uint8_t i = 0; i < config_.dataWidth; ++i)
        raw |= static_cast<uint32_t>(Wire.read()) << (8U * i);
    } else {
      for (uint8_t i = 0; i < config_.dataWidth; ++i)
        raw = (raw << 8U) | static_cast<uint32_t>(Wire.read());
    }

    if (config_.flags & SensorDriverRegistry::FLAG_SIGNED) {
      if (config_.dataWidth == 1) value = static_cast<float>(static_cast<int8_t>(raw));
      else if (config_.dataWidth == 2) value = static_cast<float>(static_cast<int16_t>(raw));
      else value = static_cast<float>(static_cast<int32_t>(raw));
    } else {
      value = static_cast<float>(raw);
    }
    quality = SensorProtocol::QUALITY_VALID;
    return true;
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

}  // namespace

uint32_t SensorDriverRegistry::crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xEDB88320U & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U))));
  }
  return crc ^ 0xFFFFFFFFU;
}

bool SensorDriverRegistry::validConfig(const DriverConfig& config) {
  if (config.sensorId == 0 || config.periodMs == 0 || config.periodMs > 86400000UL) return false;
  if (config.driverType < DRIVER_BME280 || config.driverType > DRIVER_GENERIC_I2C) return false;
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
  }
  return true;
}

SensorDriver* SensorDriverRegistry::createDriver(const DriverConfig& config) {
  switch (config.driverType) {
    case DRIVER_BME280: return new Bme280Driver();
    case DRIVER_BATTERY_ADC: return new BatteryAdcDriver();
    case DRIVER_DIGITAL_INPUT: return new DigitalInputDriver();
    case DRIVER_GENERIC_I2C: return new GenericI2cDriver();
    default: return nullptr;
  }
}

bool SensorDriverRegistry::rebuild(SensorRegistry& registry) {
  registry.clear();
  for (size_t i = 0; i < count_; ++i) {
    delete entries_[i].driver;
    entries_[i].driver = createDriver(entries_[i].config);
    if (!entries_[i].driver || !validConfig(entries_[i].config) ||
        !entries_[i].driver->begin(entries_[i].config) ||
        !registry.registerSensor(entries_[i].driver->descriptor())) {
      clear(registry);
      return false;
    }
    entries_[i].lastSampleMs = 0;
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
  entries_[index].driver = nullptr;
  if (rebuild(registry)) return true;

  count_ = previousCount;
  for (size_t i = 0; i < count_; ++i) entries_[i].config = previous[i];
  return rebuild(registry);
}

bool SensorDriverRegistry::remove(uint16_t sensorId, SensorRegistry& registry) {
  DriverConfig previous[MAX_DRIVERS]{};
  const size_t previousCount = count_;
  for (size_t i = 0; i < count_; ++i) previous[i] = entries_[i].config;

  size_t found = count_;
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].config.sensorId == sensorId) { found = i; break; }
  }
  if (found == count_) return false;
  for (size_t j = found + 1; j < count_; ++j) entries_[j - 1].config = entries_[j].config;
  --count_;
  entries_[count_] = {};
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

bool SensorDriverRegistry::load(SensorRegistry& registry) {
  Preferences prefs;
  if (!prefs.begin("sensor_cfg", true)) return false;
  const size_t got = prefs.getBytesLength("cfg");
  if (got == 0) { prefs.end(); return false; }
  if (got != sizeof(DiskConfig)) { prefs.end(); return false; }
  DiskConfig disk{};
  if (prefs.getBytes("cfg", &disk, sizeof(disk)) != sizeof(disk)) {
    prefs.end();
    return false;
  }
  prefs.end();
  if (disk.magic != CONFIG_MAGIC || disk.version != CONFIG_VERSION ||
      disk.count == 0 || disk.count > MAX_DRIVERS ||
      disk.crc32 != crc32(reinterpret_cast<const uint8_t*>(&disk),
                          offsetof(DiskConfig, crc32))) return false;

  clear(registry);
  for (size_t i = 0; i < disk.count; ++i) {
    DriverConfig config{};
    const DiskEntry& entry = disk.entries[i];
    config.driverType = entry.driverType;
    config.sensorId = entry.sensorId;
    config.pinSda = entry.pinSda;
    config.pinScl = entry.pinScl;
    config.i2cAddr = entry.i2cAddr;
    config.registerAddr = static_cast<uint16_t>(entry.reserved[0]) |
                          (static_cast<uint16_t>(entry.reserved[1]) << 8U);
    config.dataWidth = entry.reserved[2];
    config.periodMs = entry.periodMs;
    config.flags = entry.flags;
    if (!validConfig(config)) {
      clear(registry);
      return false;
    }
    entries_[count_++].config = config;
  }
  return rebuild(registry);
}

bool SensorDriverRegistry::save() const {
  DiskConfig disk{};
  disk.count = static_cast<uint8_t>(count_);
  for (size_t i = 0; i < count_; ++i) {
    const DriverConfig& config = entries_[i].config;
    DiskEntry& entry = disk.entries[i];
    entry.driverType = config.driverType;
    entry.sensorId = config.sensorId;
    entry.pinSda = config.pinSda;
    entry.pinScl = config.pinScl;
    entry.i2cAddr = config.i2cAddr;
    entry.reserved[0] = static_cast<uint8_t>(config.registerAddr);
    entry.reserved[1] = static_cast<uint8_t>(config.registerAddr >> 8U);
    entry.reserved[2] = config.dataWidth;
    entry.periodMs = config.periodMs;
    entry.flags = config.flags;
  }
  disk.crc32 = crc32(reinterpret_cast<const uint8_t*>(&disk),
                      offsetof(DiskConfig, crc32));
  Preferences prefs;
  if (!prefs.begin("sensor_cfg", false)) return false;
  const bool ok = prefs.putBytes("cfg", &disk, sizeof(disk)) == sizeof(disk);
  prefs.end();
  return ok;
}

bool SensorDriverRegistry::sample(SensorRegistry& registry, uint32_t nowMs) {
  bool ok = true;
  for (auto& entry : entries_) {
    if (!entry.driver) { ok = false; continue; }
    if (entry.lastSampleMs != 0 &&
        static_cast<uint32_t>(nowMs - entry.lastSampleMs) < entry.config.periodMs) continue;
    entry.lastSampleMs = nowMs;
    float value = 0.0f;
    uint8_t quality = SensorProtocol::QUALITY_STALE;
    const bool readOk = entry.driver->read(value, quality);
    if (!readOk && !std::isfinite(value)) value = 0.0f;
    ok &= registry.updateValue(entry.config.sensorId, value, quality);
  }
  return ok;
}

const DriverConfig* SensorDriverRegistry::config(size_t index) const {
  return index < count_ ? &entries_[index].config : nullptr;
}

String SensorDriverRegistry::listJson() const {
  String out = "[";
  for (size_t i = 0; i < count_; ++i) {
    if (i) out += ',';
    const DriverConfig& c = entries_[i].config;
    out += "{\"type\":" + String(c.driverType) +
           ",\"sensorId\":" + String(c.sensorId) +
           ",\"sda\":" + String(c.pinSda) +
           ",\"scl\":" + String(c.pinScl) +
           ",\"addr\":" + String(c.i2cAddr) +
           ",\"register\":" + String(c.registerAddr) +
           ",\"width\":" + String(c.dataWidth) +
           ",\"periodMs\":" + String(c.periodMs) +
           ",\"flags\":" + String(c.flags) + "}";
  }
  return out + "]";
}
