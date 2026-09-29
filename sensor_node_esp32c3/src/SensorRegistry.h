#pragma once

#include "SensorProtocol.h"
#include <cstddef>
#include <cstdint>

class SensorRegistry {
public:
  // driver: raised from 8 to 12 to accommodate Profile 1 (12 sensors) and
  // Profile 3 (9 sensors) without truncation. RAM impact:
  //   SensorDescriptor (62 B) × 13 = 806 B
  //   SensorValue (15 B) × 13 = 195 B
  //   valueValid_ (1 B) × 13 = 13 B
  // Total ≈ 1014 B. Safe for ESP32-C3 (no PSRAM, but 400 KB DRAM).
  static constexpr size_t MAX_SENSORS = 15;

  bool registerSensor(const SensorProtocol::SensorDescriptor& descriptor);
  bool updateValue(uint16_t id, float value, uint8_t quality);
  bool removeSensor(uint16_t id);
  void clear();

  size_t count() const { return count_; }
  const SensorProtocol::SensorDescriptor* descriptor(size_t index) const;
  const SensorProtocol::SensorValue* value(size_t index) const;
  const SensorProtocol::SensorValue* valueById(uint16_t id) const;
  int find(uint16_t id) const;

private:
  size_t count_ = 0;
  SensorProtocol::SensorDescriptor descriptors_[MAX_SENSORS]{};
  SensorProtocol::SensorValue values_[MAX_SENSORS]{};
  bool valueValid_[MAX_SENSORS]{};
};
