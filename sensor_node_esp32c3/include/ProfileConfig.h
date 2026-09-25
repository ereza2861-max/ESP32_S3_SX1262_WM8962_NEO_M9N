#pragma once

#include <cstddef>
#include <cstdint>

// FieldRadio ESP32-C3 Sensor Node — profile configuration.
//
// ARCHITECTURAL DECISION (locked by userdecisions.txt STEP 0):
//   - 4 profiles selected by a 2-bit DIP switch at boot.
//   - Pin map is NOT PHYSICALLY VALIDATED. It is a source-level contract only.
//   - Legacy battery ADC is reassigned to GPIO3 to free GPIO4 for MFRC522 MISO.
//   - Legacy digital sensor is disabled (DIGITAL_SENSOR_PIN = -1) to free GPIO10
//     for the button long-press handler; profile drivers own their own GPIOs.
//   - Buzzer uses the ESP-IDF LEDC peripheral, not blocking tone().
//
// This header is the single source of truth for the STEP 1 foundation. Profile
// sensor definitions live in ProfileSensors (STEP 3..6) and must reference the
// ID bases declared here.

namespace ProfileConfig {

enum class Profile : uint8_t {
  IslandSea = 0,
  TropicalForest = 1,
  VolcanicMountain = 2,
  SubZeroSnow = 3,
};

constexpr uint8_t PROFILE_COUNT = 4;

// --- DIP switch (2-bit) -----------------------------------------------------
// GPIO0: ADC2, safe as input after boot. GPIO1: ADC1, not a strapping pin.
// The DIP switch is read once during ProfileManager::begin(). No debounce is
// required because DIP switches are static during operation.
constexpr int DIP_BIT0_PIN = 0;
constexpr int DIP_BIT1_PIN = 1;

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
// CS on GPIO7, SCK on GPIO6, MOSI on GPIO5, MISO on GPIO4, RST on GPIO20.
// GPIO4 was the legacy battery ADC pin; it is reassigned to MFRC522 MISO.
// GPIO2 is a strapping pin and MUST NOT be used for RST.
constexpr int RFID_CS_PIN = 7;
constexpr int RFID_SCK_PIN = 6;
constexpr int RFID_MOSI_PIN = 5;
constexpr int RFID_MISO_PIN = 4;
constexpr int RFID_RST_PIN = 20;

// RFID polling and callback contract (STEP 2). These are not hardware pins;
// they are behavioral constants owned by RfidReader. Kept here so that STEP 8
// integration and any future tests reference the same numbers.
constexpr uint32_t RFID_POLL_INTERVAL_MS = 100;
constexpr uint32_t RFID_UID_FORGET_MS = 1500;
constexpr uint8_t RFID_MAX_UID_BYTES = 10;

// --- Legacy sensor pins ----------------------------------------------------
// Battery ADC moves from GPIO4 to GPIO3 (ADC1_CH3), freeing GPIO4 for RFID.
// Digital legacy sensor is disabled; profile drivers own their own pins.
constexpr int LEGACY_BATTERY_ADC_PIN = 3;
constexpr int LEGACY_DIGITAL_SENSOR_PIN = -1;

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

constexpr uint8_t RAIN_GAUGE_I2C_ADDRESS = 0x28;

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
// ARCHITECTURAL DECISION (locked by user P3-Accept-Leakage):
// GPIO3 is shared between optional DS18B20 OneWire bus and ADXL355 CS in
// Profile 3. OneWire requires external 4.7 kOhm pull-up to 3.3 V on same pin.
// When ADXL355 CS driven LOW, pull-up remains connected and leaks:
// I_leak ~= (3.3 V - V_CS_low) / R_pullup
// ~= 3.3 V / 4.7 kOhm ~= 0.70 mA
// Raises V_CS_low above ideal 0 V. ADXL355 CS V_IL must be verified against
// actual pull-up and datasheet. HARDWARE VALIDATION REQUIRED — NOT claimed
// as working.
constexpr bool PROFILE3_GPIO3_TIME_SHARED = true;
constexpr uint32_t PROFILE3_GPIO3_SETTLE_MS = 2;
constexpr uint32_t PROFILE3_ONEWIRE_PULLUP_OHMS = 4700;

constexpr uint8_t encodeDipBit(bool electricalHigh) { return electricalHigh ? 1U : 0U; }
constexpr uint8_t decodeDipBits(bool bit0High, bool bit1High) {
  return static_cast<uint8_t>(encodeDipBit(bit0High) | (encodeDipBit(bit1High) << 1));
}
constexpr bool dipValueInRange(uint8_t dipValue) { return dipValue < PROFILE_COUNT; }

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
