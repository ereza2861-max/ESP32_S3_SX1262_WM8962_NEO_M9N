#pragma once

#include "SensorProtocol.h"
#include <cstddef>
#include <cstdint>

class SensorRegistry {
public:
  static constexpr size_t MAX_SENSORS = 8;

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
