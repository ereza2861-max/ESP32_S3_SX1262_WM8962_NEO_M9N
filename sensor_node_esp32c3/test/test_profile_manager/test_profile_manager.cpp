// Native test for runtime profile and immutable pin/roster contracts.
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

void testRuntimeProfileRange() {
  assert(ProfileConfig::PROFILE_COUNT == 4);
  for (uint8_t value = 0; value < ProfileConfig::PROFILE_COUNT; ++value) {
    assert(value < ProfileConfig::PROFILE_COUNT);
  }
  assert(ProfileConfig::sensorIdBase(ProfileConfig::Profile::IslandSea) == 0x0100);
  assert(ProfileConfig::sensorIdBase(ProfileConfig::Profile::TropicalForest) == 0x0200);
  assert(ProfileConfig::sensorIdBase(ProfileConfig::Profile::VolcanicMountain) == 0x0300);
  assert(ProfileConfig::sensorIdBase(ProfileConfig::Profile::SubZeroSnow) == 0x0400);
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
  assert(ProfileConfig::RFID_SCK_PIN == 6);
  assert(ProfileConfig::RFID_MOSI_PIN == 2);
  assert(ProfileConfig::RFID_MISO_PIN == 1);
  assert(ProfileConfig::PROFILE2_WIND_VANE_ADC_PIN == 3);
  assert(ProfileConfig::PROFILE2_ADXL355_CS_PIN == 3);
  assert(ProfileConfig::PROFILE2_WIND_PULSE_PIN == 11);
  assert(ProfileConfig::PROFILE3_ONEWIRE_PIN == 3);
  assert(ProfileConfig::PROFILE3_ADXL355_CS_PIN == 3);
  assert(ProfileConfig::PROFILE3_MAX31865_CS_PIN == 11);
  assert(ProfileConfig::PROFILE3_VEML6075_I2C_ADDR == 0x10);
  assert(ProfileConfig::PROFILE3_SNOW_I2C_ADDR == 0x70);
  assert(ProfileConfig::PROFILE3_O2_I2C_ADDR == 0x73);
  assert(ProfileConfig::GPIO3_MUX_COM_PIN == 3);
  assert(ProfileConfig::GPIO3_MUX_S0_PIN == 0);
  assert(ProfileConfig::GPIO3_MUX_S1_PIN == 5);
  assert(ProfileConfig::GPIO3_MUX_S2_PIN == -1);
  assert(ProfileConfig::GPIO3_MUX_ENABLE_PIN == -1);
}

void testGpio3Contract() {
  assert(ProfileConfig::ProfileSensorsContract::gpio3IsTimeShared());
  assert(ProfileConfig::ProfileSensorsContract::gpio3OwnerWhenSamplingAdc() ==
         ProfileConfig::PROFILE2_WIND_VANE_ADC_PIN);
  assert(ProfileConfig::ProfileSensorsContract::gpio3OwnerWhenSamplingSpi() ==
         ProfileConfig::PROFILE2_ADXL355_CS_PIN);
  assert(ProfileConfig::ProfileSensorsContract::gpio3RuntimeSerializationImplemented());
  assert(ProfileConfig::ProfileSensorsContract::gpio3ExternalMuxEnabled());
  assert(ProfileConfig::ProfileSensorsContract::gpio3MuxS0Pin() == 0);
  assert(ProfileConfig::ProfileSensorsContract::gpio3MuxS1Pin() == 5);
  assert(ProfileConfig::ProfileSensorsContract::gpio3MuxComPin() == 3);
  assert(static_cast<uint8_t>(ProfileConfig::Gpio3MuxChannel::OneWire) == 0);
  assert(static_cast<uint8_t>(ProfileConfig::Gpio3MuxChannel::Adc) == 1);
  assert(static_cast<uint8_t>(ProfileConfig::Gpio3MuxChannel::Adxl355Cs) == 2);
  assert(static_cast<uint8_t>(ProfileConfig::Gpio3MuxChannel::Reserved) == 3);
}

}  // namespace

int main() {
  testRuntimeProfileRange();
  testProfileNamesDistinct();
  testSensorIdBasesDistinct();
  testProfileRosterContracts();
  testProfileInterfacesAndPins();
  testDriverTypes();
  testGpio3Contract();
  return 0;
}
