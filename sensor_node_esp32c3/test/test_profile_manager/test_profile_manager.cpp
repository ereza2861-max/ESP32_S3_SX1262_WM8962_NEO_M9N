// Native test for ProfileConfig pure DIP decoding helpers.
//
// This test does NOT depend on Arduino.h, Preferences.h, Wire.h, SPI.h, or
// any ESP-IDF headers. It only exercises constexpr logic that is compiled
// into the sensor node firmware.
//
// Run with: pio test -e native_test_profile -f test_profile_manager

#include <cassert>
#include <cstdint>
#include <cstring>

#include "ProfileConfig.h"

namespace {

void testDipBitEncoding() {
  // Electrical LOW  => logical 0 (switch closed, pulled to GND)
  // Electrical HIGH => logical 1 (switch open, pulled to VCC)
  static_assert(ProfileConfig::encodeDipBit(false) == 0, "LOW must map to 0");
  static_assert(ProfileConfig::encodeDipBit(true) == 1, "HIGH must map to 1");
}

void testDecodeAllCombinations() {
  // bit0 is LSB, bit1 is MSB.
  // (b0High, b1High) -> profile index
  assert(ProfileConfig::decodeDipBits(false, false) == 0);  // both closed
  assert(ProfileConfig::decodeDipBits(true,  false) == 1);  // bit0 open
  assert(ProfileConfig::decodeDipBits(false, true)  == 2);  // bit1 open
  assert(ProfileConfig::decodeDipBits(true,  true)  == 3);  // both open
}

void testDipRange() {
  assert(ProfileConfig::dipValueInRange(0));
  assert(ProfileConfig::dipValueInRange(1));
  assert(ProfileConfig::dipValueInRange(2));
  assert(ProfileConfig::dipValueInRange(3));
  // Values 4..255 are out of range for a 2-bit DIP.
  assert(!ProfileConfig::dipValueInRange(4));
  assert(!ProfileConfig::dipValueInRange(255));
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

}  // namespace

int main() {
  testDipBitEncoding();
  testDecodeAllCombinations();
  testDipRange();
  testProfileNamesDistinct();
  testSensorIdBasesDistinct();
  return 0;
}
