#pragma once

#include <Arduino.h>
#include "SensorDriver.h"
#include "SensorRegistry.h"
#include <cstddef>
#include <cstdint>

class SensorDriverRegistry {
public:
  // Capacity includes the largest profile roster plus the global RFID descriptor.
  static constexpr size_t MAX_DRIVERS = 15;
  static constexpr uint8_t DRIVER_BME280 = 1;
  static constexpr uint8_t DRIVER_BATTERY_ADC = 2;
  static constexpr uint8_t DRIVER_DIGITAL_INPUT = 3;
  static constexpr uint8_t DRIVER_GENERIC_I2C = 4;
  static constexpr uint8_t DRIVER_GENERIC_ADC = 5;
  static constexpr uint8_t DRIVER_GENERIC_UART = 6;
  static constexpr uint8_t DRIVER_ATLAS_EZO = 7;
  static constexpr uint8_t DRIVER_ONEWIRE_TEMP = 8;
  static constexpr uint8_t DRIVER_PULSE_COUNTER = 9;
  static constexpr uint8_t DRIVER_GAS_ADC = 10;
  static constexpr uint8_t DRIVER_SEISMIC_SPI = 11;
  static constexpr uint8_t DRIVER_DUST_VISIBILITY = 12;
  static constexpr uint16_t FLAG_SIGNED = 1U << 0;
  static constexpr uint16_t FLAG_LITTLE_ENDIAN = 1U << 1;
  static constexpr uint16_t FLAG_REGISTER_16BIT = 1U << 2;

  bool add(const DriverConfig& config, SensorRegistry& registry);
  void clear(SensorRegistry& registry);
  bool sample(SensorRegistry& registry, uint32_t nowMs);
  size_t count() const { return count_; }
  uint32_t minimumPeriodMs() const;
  bool setSamplingPeriod(uint16_t sensorId, uint32_t periodMs, SensorRegistry& registry);

  struct OneWireRomInfo {
    uint16_t sensorId = 0;
    uint8_t rom[8] = {};
  };
  bool bindOneWireRom(uint16_t sensorId, uint8_t index);
  size_t getOneWireRomList(OneWireRomInfo* out, size_t maxEntries) const;

private:
  struct Entry {
    DriverConfig config{};
    SensorDriver* driver = nullptr;
    uint32_t lastSampleMs = 0;
    uint32_t sourceSequence = 0;
    uint8_t consecutiveReadFailures = 0;
    bool calibrationDegraded = false;
    SensorProtocol::SensorDescriptor descriptor{};
  };

  Entry entries_[MAX_DRIVERS]{};
  size_t count_ = 0;

  static SensorDriver* createDriver(const DriverConfig& config);
  static bool validConfig(const DriverConfig& config);
  bool rebuild(SensorRegistry& registry);
  static bool loadCalibration(uint16_t sensorId, float& scale, float& offset, bool& invalid);
  static bool allocateSourceSequenceBlock(uint16_t sensorId, uint32_t& firstSequence);
};
