#include "SensorRegistry.h"

bool SensorRegistry::registerSensor(const SensorProtocol::SensorDescriptor& descriptor) {
  if (!SensorProtocol::validDescriptor(descriptor)) return false;
  const int existing = find(descriptor.id);
  if (existing >= 0) {
    descriptors_[existing] = descriptor;
    return true;
  }
  if (count_ >= MAX_SENSORS) return false;
  descriptors_[count_] = descriptor;
  values_[count_] = {};
  valueValid_[count_] = false;
  ++count_;
  return true;
}

bool SensorRegistry::updateValue(uint16_t id, float value, uint8_t quality) {
  if (id == 0 || !std::isfinite(value)) return false;
  const int index = find(id);
  if (index < 0) return false;
  SensorProtocol::SensorValue sample{};
  sample.id = id;
  sample.value = value;
  sample.timestamp = 0;
  sample.quality = quality;
  values_[index] = sample;
  valueValid_[index] = true;
  return true;
}

bool SensorRegistry::removeSensor(uint16_t id) {
  const int index = find(id);
  if (index < 0) return false;
  const size_t i = static_cast<size_t>(index);
  for (size_t j = i + 1; j < count_; ++j) {
    descriptors_[j - 1] = descriptors_[j];
    values_[j - 1] = values_[j];
    valueValid_[j - 1] = valueValid_[j];
  }
  --count_;
  descriptors_[count_] = {};
  values_[count_] = {};
  valueValid_[count_] = false;
  return true;
}

void SensorRegistry::clear() {
  count_ = 0;
  for (size_t i = 0; i < MAX_SENSORS; ++i) {
    descriptors_[i] = {};
    values_[i] = {};
    valueValid_[i] = false;
  }
}

const SensorProtocol::SensorDescriptor* SensorRegistry::descriptor(size_t index) const {
  return index < count_ ? &descriptors_[index] : nullptr;
}

const SensorProtocol::SensorValue* SensorRegistry::value(size_t index) const {
  return index < count_ && valueValid_[index] ? &values_[index] : nullptr;
}

const SensorProtocol::SensorValue* SensorRegistry::valueById(uint16_t id) const {
  const int index = find(id);
  return index >= 0 ? value(static_cast<size_t>(index)) : nullptr;
}

int SensorRegistry::find(uint16_t id) const {
  if (id == 0) return -1;
  for (size_t i = 0; i < count_; ++i) {
    if (descriptors_[i].id == id) return static_cast<int>(i);
  }
  return -1;
}
