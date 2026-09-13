#pragma once
#include <Arduino.h>

/*
 * Target: ESP32-S3-WROOM-1 + SX1262 + WM8962 + NEO-M9N + microSD.
 *
 * IMPORTANT: this is a new S3 routing map. It is NOT electrically compatible
 * with the old ESP32-WROOM-32E PCB without PCB trace/net changes.
 * GPIO19/20 are reserved for the ESP32-S3 native USB D-/D+.
 */
namespace Board {
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;

// WM8962 I2S
// ESP32 -> WM8962: DOUT = codec DACDAT
// WM8962 -> ESP32: DIN  = codec ADCDAT
constexpr int I2S_BCLK = 4;
constexpr int I2S_LRCLK = 5;
constexpr int I2S_DOUT = 6;
constexpr int I2S_DIN  = 7;

// Shared SPI bus: SX1262 + microSD
constexpr int SPI_SCK  = 12;
constexpr int SPI_MISO = 13;
constexpr int SPI_MOSI = 11;

constexpr int LORA_CS   = 10; // SX1262 NSS
constexpr int LORA_RST  = 14;
constexpr int LORA_DIO1 = 2; // SX1262 DIO1 IRQ
constexpr int LORA_BUSY = 15; // SX1262 BUSY (mandatory for SX126x)

constexpr int SD_CS = 16;

// NEO-M9N UART
constexpr int GNSS_RX = 18; // ESP32-S3 RX <- NEO-M9N TX
constexpr int GNSS_TX = 17; // ESP32-S3 TX -> NEO-M9N RX
constexpr uint32_t GNSS_BAUD = 38400;

// Auxiliary Rev-B controls. These nets MUST be physically routed on the PCB.
// PTT/SOS remain on dedicated inputs; the new GPIO38..42 block is reserved
// for the requested buzzer/RGB/haptic/charge/TX indicators.
constexpr int BTN_PTT = 21;       // active-low, INPUT_PULLUP
constexpr int BTN_SOS = 47;       // active-low, INPUT_PULLUP
constexpr int BATTERY_ADC = 1;    // battery divider ADC input
constexpr int BUZZER = 38;        // active-high buzzer; passive buzzer needs PWM hardware
constexpr int LED_RGB = 39;       // one-wire/addressable RGB data
constexpr int HAPTIC = 40;        // active-high haptic driver enable
constexpr int LED_CHARGING = 41;  // charging-status LED; driven only from charger heuristic
constexpr int LED_TX = 42;        // dedicated TX indicator
constexpr int LED_RX = 48;        // dedicated RX indicator
constexpr int STATUS_LED = -1;    // removed: do not alias status onto another function

// ESP32-S3 native USB uses GPIO19=D- and GPIO20=D+.
constexpr int USB_D_MINUS = 19;
constexpr int USB_D_PLUS = 20;

// WM8962 uses an external 24 MHz oscillator on the PCB.
// No ESP32 GPIO is assigned to MCLK.
constexpr uint8_t WM8962_I2C_ADDR = 0x1A;
constexpr uint32_t WM8962_MCLK_HZ = 24000000UL;

// Catch accidental future pin aliasing at compile time.
constexpr bool pinsUnique() {
  constexpr int pins[] = {
      I2C_SDA, I2C_SCL, I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_DIN,
      SPI_SCK, SPI_MISO, SPI_MOSI, LORA_CS, LORA_RST, LORA_DIO1,
      LORA_BUSY, SD_CS, GNSS_RX, GNSS_TX, BTN_PTT, BTN_SOS, BATTERY_ADC,
      BUZZER, LED_RGB, HAPTIC, LED_CHARGING, LED_TX, LED_RX,
      USB_D_MINUS, USB_D_PLUS
  };
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    if (pins[i] < 0) continue;
    for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j)
      if (pins[i] == pins[j]) return false;
  }
  return true;
}
static_assert(pinsUnique(), "BoardConfig GPIO collision detected");
}
