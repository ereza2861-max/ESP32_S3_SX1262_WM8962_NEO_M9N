#pragma once

#include <cstddef>
#include <cstdint>

// FieldRadio ESP32-C3 Sensor Node — profile configuration.
//
// ARCHITECTURAL DECISION:
//   - 4 profiles are selected by runtime configuration persisted in NVS and changed through WebUI.
//   - There is no physical profile selector; GPIO4/GPIO5/GPIO6 are fully available to sensor functions.
//   - Pin map is NOT PHYSICALLY VALIDATED. It is a source-level contract only.
//   - Buzzer uses the ESP-IDF LEDC peripheral, not blocking tone().
//
// This header is the single source of truth for the final architecture foundation. Profile
// sensor definitions live in ProfileSensors and must reference the
// ID bases declared here.

namespace ProfileConfig {

enum class Profile : uint8_t {
  IslandSea = 0,
  TropicalForest = 1,
  VolcanicMountain = 2,
  SubZeroSnow = 3,
};

constexpr uint8_t PROFILE_COUNT = 4;

// --- Runtime profile configuration -----------------------------------------
// Profile selection is persisted in NVS namespace "sensor" under key "profile".
// No physical selector pins are reserved.

// --- Button (long-press 1.5 s toggles Wi-Fi AP) ----------------------------
// GPIO10 is free, not USB-JTAG, and not used by any profile driver.
constexpr int BUTTON_PIN = 10;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 1500;

// --- Buzzer ----------------------------------------------------------------
// GPIO21: free, not a strapping pin, and safe for LEDC PWM.
constexpr int BUZZER_PIN = 21;
constexpr uint32_t BUZZER_FREQ_HZ = 2000;
constexpr uint32_t BUZZER_DURATION_MS = 200;
constexpr uint8_t BUZZER_LEDC_CHANNEL = 0;
constexpr uint8_t BUZZER_LEDC_RESOLUTION_BITS = 10;

// --- MFRC522 RFID (SPI) ----------------------------------------------------
// CS on GPIO7, SCK on GPIO6, MOSI on GPIO2, MISO on GPIO1, RST on GPIO20.
// GPIO2 is a strapping pin and is used as an output only after boot.
// GPIO1 is used as an input after boot.
constexpr int RFID_CS_PIN = 7;
constexpr int RFID_SCK_PIN = 6;
constexpr int RFID_MOSI_PIN = 2;
constexpr int RFID_MISO_PIN = 1;
constexpr int RFID_RST_PIN = 20;

// RFID polling and callback contract (RFID). These are not hardware pins;
// they are behavioral constants owned by RfidReader. Kept here so that integration any future tests reference the same numbers.
constexpr uint32_t RFID_POLL_INTERVAL_MS = 100;
constexpr uint32_t RFID_UID_FORGET_MS = 1500;
constexpr uint8_t RFID_MAX_UID_BYTES = 10;

// --- Stable sensor ID bases per profile ------------------------------------
// Each profile owns a 256-ID window so IDs are unique, stable across reboots,
// and traceable to a profile without touching the BLE wire contract.
constexpr uint16_t SENSOR_ID_BASE_ISLAND_SEA = 0x0100;
constexpr uint16_t SENSOR_ID_BASE_TROPICAL_FOREST = 0x0200;
constexpr uint16_t SENSOR_ID_BASE_VOLCANIC_MOUNTAIN = 0x0300;
constexpr uint16_t SENSOR_ID_BASE_SUB_ZERO_SNOW = 0x0400;
constexpr uint16_t SENSOR_ID_BASE_RFID = 0x00F0;
constexpr uint16_t SENSOR_ID_BASE_BUTTON = 0x00F1;
constexpr uint16_t SENSOR_ID_RFID_EVENT = SENSOR_ID_BASE_RFID;  // 0x00F0

// --- Interface kinds -------------------------------------------------------
enum class InterfaceKind : uint8_t {
  I2C = 0, SPI = 1, UART = 2, OneWire = 3, Adc = 4, Pulse = 5, Digital = 6,
};

// --- Driver type tags ------------------------------------------------------
enum class DriverType : uint8_t {
  Bme280 = 1, BatteryAdc = 2, DigitalInput = 3, GenericI2c = 4,
  GenericAdc = 5, GenericUart = 6, AtlasEzo = 7, OneWireTemp = 8,
  PulseCounter = 9,
};

constexpr uint16_t SENSOR_ID_BASE_BATTERY = 0x00F2;

constexpr uint16_t sensorIdBase(Profile profile) {
  switch (profile) {
    case Profile::IslandSea:         return SENSOR_ID_BASE_ISLAND_SEA;
    case Profile::TropicalForest:    return SENSOR_ID_BASE_TROPICAL_FOREST;
    case Profile::VolcanicMountain:  return SENSOR_ID_BASE_VOLCANIC_MOUNTAIN;
    case Profile::SubZeroSnow:       return SENSOR_ID_BASE_SUB_ZERO_SNOW;
  }
  return 0;
}

constexpr int PROFILE0_I2C_SDA_PIN = 8;
constexpr int PROFILE0_I2C_SCL_PIN = 9;
constexpr uint32_t PROFILE0_I2C_HZ = 100000UL;
constexpr int PROFILE0_WAVE_ADC_PIN = 3;
constexpr int PROFILE0_TURBIDITY_ADC_PIN = 4;
constexpr int PROFILE0_ONEWIRE_PIN = 3;

constexpr int PROFILE1_I2C_SDA_PIN = 8;
constexpr int PROFILE1_I2C_SCL_PIN = 9;
constexpr uint32_t PROFILE1_I2C_HZ = 100000UL;
constexpr int PROFILE1_ONEWIRE_PIN = 3;
constexpr int PROFILE1_CH4_ADC_PIN = 4;
constexpr int PROFILE1_TEROS_UART_RX_PIN = -1;
constexpr int PROFILE1_TEROS_UART_TX_PIN = -1;
constexpr int PROFILE1_PYRA_UART_RX_PIN = -1;
constexpr int PROFILE1_PYRA_UART_TX_PIN = -1;

constexpr int PROFILE2_I2C_SDA_PIN = 8;
constexpr int PROFILE2_I2C_SCL_PIN = 9;
constexpr uint32_t PROFILE2_I2C_HZ = 100000UL;
constexpr int PROFILE2_H2S_ADC_PIN = 4;
constexpr int PROFILE2_WIND_VANE_ADC_PIN = 3;
constexpr int PROFILE2_WIND_PULSE_PIN = 11;
constexpr int PROFILE2_PMS_UART_NUM = 1;
constexpr int PROFILE2_PMS_RX_PIN = 18;
constexpr int PROFILE2_PMS_TX_PIN = 19;
constexpr int PROFILE2_ADXL355_CS_PIN = 3;
constexpr uint32_t PROFILE2_ADC_SETTLE_MS = 2;

constexpr uint8_t RAIN_GAUGE_I2C_ADDRESS = 0x28;  // PLACEHOLDER; verify HW
constexpr uint8_t ATLAS_EZO_EC_I2C_ADDR = 0x64;  // PLACEHOLDER; verify HW
constexpr uint8_t ATLAS_EZO_PH_I2C_ADDR = 0x63;  // PLACEHOLDER; verify HW
constexpr uint8_t SCD4X_I2C_ADDR = 0x62;         // PLACEHOLDER; verify HW

constexpr int PROFILE3_I2C_SDA_PIN = 8;
constexpr int PROFILE3_I2C_SCL_PIN = 9;
constexpr uint32_t PROFILE3_I2C_HZ = 100000UL;
constexpr int PROFILE3_ONEWIRE_PIN = 3;
constexpr int PROFILE3_ADXL355_CS_PIN = 3;
constexpr int PROFILE3_MAX31865_CS_PIN = 11;
constexpr int PROFILE3_UART_NUM = 1;
constexpr int PROFILE3_UART_RX_PIN = 18;
constexpr int PROFILE3_UART_TX_PIN = 19;
constexpr uint8_t PROFILE3_VEML6075_I2C_ADDR = 0x10;
constexpr uint8_t PROFILE3_O2_I2C_ADDR = 0x73;
constexpr uint8_t PROFILE3_SNOW_I2C_ADDR = 0x70;
constexpr uint8_t PROFILE3_BME280_I2C_ADDR = 0x76;
constexpr uint16_t PROFILE3_VEML6075_REG_UVA = 0;
constexpr uint16_t PROFILE3_VEML6075_REG_UVB = 1;
constexpr uint16_t PROFILE3_VEML6075_REG_UVI = 2;
// ARCHITECTURAL DECISION: the Profile 3 OneWire bus and ADXL355 CS share
// the time-share pin. OneWire requires an external 4.7 kOhm pull-up.
// HARDWARE VALIDATION REQUIRED — source-level contract only.
constexpr bool PROFILE3_TIME_SHARE_ENABLED = true;
constexpr uint32_t PROFILE3_TIME_SHARE_SETTLE_MS = 2;
constexpr uint32_t PROFILE3_ONEWIRE_PULLUP_OHMS = 4700;

constexpr size_t expectedSensorCount(Profile profile) {
  switch (profile) {
    case Profile::IslandSea: return 6;
    case Profile::TropicalForest: return 12;
    case Profile::VolcanicMountain: return 8;
    case Profile::SubZeroSnow: return 9;
  }
  return 0;
}

constexpr uint16_t sensorIdFor(Profile profile, size_t index) {
  const uint16_t base = sensorIdBase(profile);
  return (index < expectedSensorCount(profile)) ? static_cast<uint16_t>(base + index) : 0;
}

namespace ProfileSensorsContract {
constexpr bool gpio3IsTimeShared() { return true; }
constexpr int gpio3OwnerWhenSamplingAdc() { return PROFILE2_WIND_VANE_ADC_PIN; }
constexpr int gpio3OwnerWhenSamplingSpi() { return PROFILE2_ADXL355_CS_PIN; }
constexpr bool gpio3RuntimeSerializationImplemented() { return true; }
}

constexpr const char* profileName(Profile profile) {
  switch (profile) {
    case Profile::IslandSea:         return "island_sea";
    case Profile::TropicalForest:    return "tropical_forest";
    case Profile::VolcanicMountain:  return "volcanic_mountain";
    case Profile::SubZeroSnow:       return "sub_zero_snow";
  }
  return "unknown";
}

}  // namespace ProfileConfig
