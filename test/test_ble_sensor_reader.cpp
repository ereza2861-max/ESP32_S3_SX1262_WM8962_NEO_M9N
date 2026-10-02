#include <cassert>
#include <cmath>
#include <cstring>
#include "SensorProtocol.h"

void test_ble_sensor_reader_contract() {
  static_assert(sizeof(SensorProtocol::SensorDescriptor) == 63,
                "BLE descriptor wire layout changed");
  static_assert(sizeof(SensorProtocol::SensorDescriptorResponse) == 68,
                "BLE descriptor response wire layout changed");
  static_assert(sizeof(SensorProtocol::SensorValue) == 20,
                "BLE sensor value wire layout changed");

  SensorProtocol::SensorDescriptor descriptor{};
  descriptor.id = 1;
  std::strncpy(descriptor.name, "temperature", sizeof(descriptor.name) - 1);
  std::strncpy(descriptor.unit, "degC", sizeof(descriptor.unit) - 1);
  descriptor.min = -40.0f;
  descriptor.max = 125.0f;
  assert(SensorProtocol::validDescriptor(descriptor));

  SensorProtocol::SensorValue value{};
  value.id = 1;
  value.value = 25.0f;
  assert(SensorProtocol::validValue(value));
  value.value = NAN;
  assert(!SensorProtocol::validValue(value));
}

int main() {
  test_ble_sensor_reader_contract();
  return 0;
}
