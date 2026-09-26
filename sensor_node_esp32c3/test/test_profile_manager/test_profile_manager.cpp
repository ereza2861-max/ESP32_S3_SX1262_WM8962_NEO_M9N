// Native test for ProfileConfig pure profile selector decoding helpers.
//
// This test does NOT depend on Arduino.h, Preferences.h, Wire.h, SPI.h, or
// any ESP-IDF headers. It only exercises constexpr logic that is compiled
// into the sensor node firmware.
//
// Run with: pio test -e native_test_profile -f test_profile_manager

#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "ProfileConfig.h"
#include "SensorRegistry.h"

namespace {

void testDipBitEncoding() {
  // Electrical LOW  => logical 0 (switch closed, pulled to GND)
  // Electrical HIGH => logical 1 (switch open, pulled to VCC)
  static_assert(ProfileConfig::encodeProfileSelectorBit(false) == 0, "LOW must map to 0");
  static_assert(ProfileConfig::encodeProfileSelectorBit(true) == 1, "HIGH must map to 1");
}

void testDecodeAllCombinations() {
  // bit0 is LSB, bit1 is MSB.
  // (b0High, b1High) -> profile index
  assert(ProfileConfig::decodeProfileSelectorBits(false, false, false) == 0);
  assert(ProfileConfig::decodeProfileSelectorBits(true,  false, false) == 1);
  assert(ProfileConfig::decodeProfileSelectorBits(false, true,  false) == 2);
  assert(ProfileConfig::decodeProfileSelectorBits(true,  true,  false)  == 3);
  assert(ProfileConfig::decodeProfileSelectorBits(false, false, true) == 4);
  assert(ProfileConfig::decodeProfileSelectorBits(true,  false, true) == 5);
  assert(ProfileConfig::decodeProfileSelectorBits(false, true,  true)  == 6);
  assert(ProfileConfig::decodeProfileSelectorBits(true,  true,  true)  == 7);
}

void testDipRange() {
  assert(ProfileConfig::profileSelectorValueInRange(0));
  assert(ProfileConfig::profileSelectorValueInRange(1));
  assert(ProfileConfig::profileSelectorValueInRange(2));
  assert(ProfileConfig::profileSelectorValueInRange(3));
  // Values 4..7 are reserved selector encodings; 8..255 are invalid.
  assert(!ProfileConfig::profileSelectorValueInRange(4));
  assert(!ProfileConfig::profileSelectorValueInRange(255));
}

void testProfileNamesDistinct() {
  // Sanity: the 4 profile names must be distinct non-null strings.
  const char* a = ProfileConfig::profileName(ProfileConfig::Profile::IslandSea);
  const char* b = ProfileConfig::profileName(ProfileConfig::Profile::TropicalForest);
  const char* c = ProfileConfig::profileName(ProfileConfig::Profile::VolcanicMountain);
  const char* d = ProfileConfig::profileName(ProfileConfig::Profile::SubZeroSnow);
  assert(a && b && c && d);
  assert(std::strcmp(a, b) != 0 && std::strcmp(a, c) != 0 &&
         std::strcmp(a, d) != 0 && std::strcmp(b, c) != 0 &&
         std::strcmp(b, d) != 0 && std::strcmp(c, d) != 0);
}

void testSensorIdBasesDistinct() {
  // Sensor ID bases must be stable and distinct so profile IDs never collide.
  assert(ProfileConfig::SENSOR_ID_BASE_ISLAND_SEA == 0x0100);
  assert(ProfileConfig::SENSOR_ID_BASE_TROPICAL_FOREST == 0x0200);
  assert(ProfileConfig::SENSOR_ID_BASE_VOLCANIC_MOUNTAIN == 0x0300);
  assert(ProfileConfig::SENSOR_ID_BASE_SUB_ZERO_SNOW == 0x0400);
  assert(ProfileConfig::SENSOR_ID_BASE_RFID == 0x00F0);
  assert(ProfileConfig::SENSOR_ID_BASE_BUTTON == 0x00F1);
  assert(ProfileConfig::SENSOR_ID_BASE_BATTERY == 0x00F2);
}

void testProfileRosterContracts() {
  using Profile = ProfileConfig::Profile;
  assert(ProfileConfig::expectedSensorCount(Profile::IslandSea) == 6);
  assert(ProfileConfig::expectedSensorCount(Profile::TropicalForest) == 12);
  assert(ProfileConfig::expectedSensorCount(Profile::VolcanicMountain) == 8);
  assert(ProfileConfig::expectedSensorCount(Profile::SubZeroSnow) == 9);

  for (Profile profile : {Profile::IslandSea, Profile::TropicalForest,
                          Profile::VolcanicMountain, Profile::SubZeroSnow}) {
    const size_t count = ProfileConfig::expectedSensorCount(profile);
    for (size_t i = 0; i < count; ++i) {
      const uint16_t id = ProfileConfig::sensorIdFor(profile, i);
      assert(id != 0);
      assert(id == static_cast<uint16_t>(ProfileConfig::sensorIdBase(profile) + i));
      for (size_t j = i + 1; j < count; ++j) {
        assert(id != ProfileConfig::sensorIdFor(profile, j));
      }
    }
    assert(count <= SensorRegistry::MAX_SENSORS);
  }
}

void testDriverTypes() {
  using Driver = ProfileConfig::DriverType;
  assert(static_cast<uint8_t>(Driver::Bme280) == 1);
  assert(static_cast<uint8_t>(Driver::BatteryAdc) == 2);
  assert(static_cast<uint8_t>(Driver::DigitalInput) == 3);
  assert(static_cast<uint8_t>(Driver::GenericI2c) == 4);
  assert(static_cast<uint8_t>(Driver::GenericAdc) == 5);
  assert(static_cast<uint8_t>(Driver::GenericUart) == 6);
  assert(static_cast<uint8_t>(Driver::AtlasEzo) == 7);
  assert(static_cast<uint8_t>(Driver::OneWireTemp) == 8);
  assert(static_cast<uint8_t>(Driver::PulseCounter) == 9);
}

void testProfileInterfacesAndPins() {
  using Interface = ProfileConfig::InterfaceKind;
  assert(static_cast<uint8_t>(Interface::I2C) == 0);
  assert(static_cast<uint8_t>(Interface::SPI) == 1);
  assert(static_cast<uint8_t>(Interface::UART) == 2);
  assert(static_cast<uint8_t>(Interface::OneWire) == 3);
  assert(static_cast<uint8_t>(Interface::Adc) == 4);
  assert(static_cast<uint8_t>(Interface::Pulse) == 5);
  assert(ProfileConfig::PROFILE2_WIND_VANE_ADC_PIN == 15);
  assert(ProfileConfig::PROFILE2_ADXL355_CS_PIN == 15);
  assert(ProfileConfig::PROFILE2_WIND_PULSE_PIN == 11);
  assert(ProfileConfig::PROFILE3_ONEWIRE_PIN == 15);
  assert(ProfileConfig::PROFILE3_ADXL355_CS_PIN == 15);
  assert(ProfileConfig::PROFILE3_MAX31865_CS_PIN == 11);
  assert(ProfileConfig::PROFILE3_VEML6075_I2C_ADDR == 0x10);
  assert(ProfileConfig::PROFILE3_SNOW_I2C_ADDR == 0x70);
  assert(ProfileConfig::PROFILE3_O2_I2C_ADDR == 0x73);
}

void testGpio15Contract() {
  assert(ProfileConfig::ProfileSensorsContract::gpio15IsTimeSharedInProfile2());
  assert(ProfileConfig::ProfileSensorsContract::gpio15OwnerWhenSamplingAdc() ==
         ProfileConfig::PROFILE2_WIND_VANE_ADC_PIN);
  assert(ProfileConfig::ProfileSensorsContract::gpio15OwnerWhenSamplingSpi() ==
         ProfileConfig::PROFILE2_ADXL355_CS_PIN);
  assert(ProfileConfig::ProfileSensorsContract::gpio15RuntimeSerializationImplemented());
}

}  // namespace

int main() {
  testDipBitEncoding();
  testDecodeAllCombinations();
  testDipRange();
  testProfileNamesDistinct();
  testSensorIdBasesDistinct();
  testProfileRosterContracts();
  testProfileInterfacesAndPins();
  testDriverTypes();
  testGpio15Contract();
  return 0;
}
