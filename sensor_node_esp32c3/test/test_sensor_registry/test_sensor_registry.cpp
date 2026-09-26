// Native test for SensorRegistry portable behavior.
//
// SensorRegistry itself does not depend on Preferences, Wire, SPI, or WiFi.
// It only uses SensorProtocol::SensorDescriptor / SensorValue and standard
// C++ containers. This test therefore links against the registry's source
// file only (see platformio.ini for the native_test_registry env).

#include <cassert>
#include <cstdint>
#include <cmath>

#include "SensorRegistry.h"

namespace {

SensorProtocol::SensorDescriptor makeDescriptor(uint16_t id) {
  SensorProtocol::SensorDescriptor d{};
  d.id = id;
  d.type = static_cast<uint8_t>(SensorProtocol::SensorType::GENERIC);
  d.name[0] = 't';
  d.name[1] = '\0';
  d.unit[0] = 'u';
  d.unit[1] = '\0';
  d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
  d.scale = 1.0f;
  d.offset = 0.0f;
  d.min = 0.0f;
  d.max = 100.0f;
  d.periodMs = 1000;
  d.flags = SensorProtocol::FLAG_ENABLED;
  return d;
}

void testRegisterAndCount() {
  SensorRegistry reg;
  assert(reg.count() == 0);
  assert(reg.registerSensor(makeDescriptor(1)));
  assert(reg.registerSensor(makeDescriptor(2)));
  assert(reg.count() == 2);
  // Duplicate ID updates, does not grow.
  assert(reg.registerSensor(makeDescriptor(1)));
  assert(reg.count() == 2);
}

void testFindAndValueById() {
  SensorRegistry reg;
  assert(reg.registerSensor(makeDescriptor(0x0100)));
  assert(reg.updateValue(0x0100, 42.0f, SensorProtocol::QUALITY_VALID));
  const auto* v = reg.valueById(0x0100);
  assert(v && v->id == 0x0100);
  assert(v->value == 42.0f);
  assert(v->quality == SensorProtocol::QUALITY_VALID);
  assert(reg.find(0x0100) == 0);
  assert(reg.find(0x9999) == -1);
}

void testRemoveSensor() {
  SensorRegistry reg;
  assert(reg.registerSensor(makeDescriptor(1)));
  assert(reg.registerSensor(makeDescriptor(2)));
  assert(reg.registerSensor(makeDescriptor(3)));
  assert(reg.removeSensor(2));
  assert(reg.count() == 2);
  assert(reg.find(2) == -1);
  // Order preserved for remaining entries.
  assert(reg.descriptor(0)->id == 1);
  assert(reg.descriptor(1)->id == 3);
}

void testMaxCapacity() {
  // Register exactly MAX_SENSORS entries, verify the next insert fails.
  SensorRegistry reg;
  for (size_t i = 0; i < SensorRegistry::MAX_SENSORS; ++i) {
    assert(reg.registerSensor(makeDescriptor(static_cast<uint16_t>(i + 1))));
  }
  assert(reg.count() == SensorRegistry::MAX_SENSORS);
  assert(!reg.registerSensor(makeDescriptor(0xFFFF)));
  assert(reg.count() == SensorRegistry::MAX_SENSORS);
}

void testInvalidValueRejected() {
  SensorRegistry reg;
  assert(reg.registerSensor(makeDescriptor(1)));
  // id 0 must be rejected.
  assert(!reg.updateValue(0, 1.0f, SensorProtocol::QUALITY_VALID));
  // Unknown id must be rejected.
  assert(!reg.updateValue(0x9999, 1.0f, SensorProtocol::QUALITY_VALID));
  // NaN must be rejected.
  #if defined(_MSC_VER)
  const float nanValue = std::nanf("");
#else
  const float nanValue = __builtin_nanf("");
#endif
  assert(!reg.updateValue(1, nanValue, SensorProtocol::QUALITY_VALID));
}

}  // namespace

int main() {
  testRegisterAndCount();
  testFindAndValueById();
  testRemoveSensor();
  testMaxCapacity();
  testInvalidValueRejected();
  return 0;
}
