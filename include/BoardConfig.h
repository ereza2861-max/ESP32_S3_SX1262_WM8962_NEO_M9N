#pragma once
#include <Arduino.h>

#if !defined(FIELD_RADIO_ESP32_S3_WROOM_1_N16R8)
#error "This firmware is pinned to ESP32-S3-WROOM-1-N16R8"
#endif
#if !defined(BOARD_HAS_PSRAM)
#error "ESP32-S3-WROOM-1-N16R8 requires PlatformIO PSRAM support"
#endif

/*
 * Target: ESP32-S3-WROOM-1-N16R8 (16 MB Quad Flash + 8 MB Octal PSRAM)
 *         + SX1262 + WM8962 + NEO-M9N + microSD.
 *
 * The PlatformIO environment must configure QIO flash + OPI PSRAM for this
 * exact module variant. Do not silently substitute an N16R2 or no-PSRAM board.
 *
 * IMPORTANT: this is a new S3 routing map. It is NOT electrically compatible
 * with the old ESP32-WROOM-32E PCB without PCB trace/net changes.
 * GPIO19/20 are reserved for the ESP32-S3 native USB D-/D+.
 */
namespace Board {
constexpr int I2C_SDA = 38;
constexpr int I2C_SCL = 48;

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
constexpr int LORA_RST  = 17; // SX1262 RESET; moved off GPIO14
constexpr int LORA_DIO1 = 14; // SX1262 DIO1 IRQ; RTC-capable, non-strapping
constexpr int LORA_BUSY = 15; // SX1262 BUSY (mandatory for SX126x)

constexpr int SD_CS = 16; // microSD CS; secondary SPI chip-select

// NEO-M9N UART
constexpr int GNSS_RX = 44; // ESP32-S3 RX <- NEO-M9N TX
constexpr int GNSS_TX = 43; // ESP32-S3 TX -> NEO-M9N RX
constexpr uint32_t GNSS_BAUD = 38400;

// Auxiliary Rev-C controls. These nets MUST be physically routed on the PCB.
// PTT/SOS are active-high RTC inputs for deep-sleep wake. Use an external
// pulldown (47 kOhm recommended) to GND and a normally-open pushbutton to 3V3;
// add a local 100 nF capacitor from each input to GND for debounce/noise
// suppression. Do not use an external pullup with active-high logic: that
// would make the idle state HIGH and invert the requested polarity.
constexpr int BTN_PTT = 21;       // active-high, RTC wake, external pulldown
constexpr int BTN_SOS = 18;       // active-high, RTC wake, external pulldown
constexpr int BATTERY_ADC = 1;    // battery divider ADC input

// MAX2016 detector outputs. Keep both analogue measurements on ADC1 so
// they remain usable while Wi-Fi is active. GPIO8 is repurposed from I2C SDA.
constexpr int MAX2016_OUT_FWD = 2;
constexpr int MAX2016_OUT_REF = 8;

// External 32-KB I2C FRAM (MB85RC256V), sharing the codec I2C bus.
// GPIO48 is a normal GPIO on ESP32-S3-WROOM-1 and is routed through the GPIO matrix.
constexpr int FRAM_SDA = I2C_SDA;
constexpr int FRAM_SCL = I2C_SCL;
constexpr int BUZZER = 47;        // active-high buzzer; passive buzzer needs PWM hardware
constexpr int LED_RGB = 39;       // one-wire/addressable RGB data
constexpr int HAPTIC = 40;        // active-high haptic driver enable
constexpr int LED_CHARGING = 41;  // charging-status LED; driven only from charger heuristic
constexpr int LED_TX = 42;        // dedicated TX indicator
constexpr int LED_RX = -1;        // RX is already indicated by the addressable RGB LED; GPIO48 is reserved for I2C SCL
constexpr int STATUS_LED = -1;    // removed: do not alias status onto another function

// ESP32-S3 native USB uses GPIO19=D- and GPIO20=D+.
constexpr int USB_D_MINUS = 19;
constexpr int USB_D_PLUS = 20;

// WM8962 uses an external 24 MHz oscillator on the PCB.
// No ESP32 GPIO is assigned to MCLK.
constexpr uint8_t WM8962_I2C_ADDR = 0x1A;
constexpr uint32_t WM8962_MCLK_HZ = 24000000UL;

// Rev-C routing source of truth: keep this map synchronized with docs/PCB_MAPPING.md.
// Catch accidental future pin aliasing at compile time.
constexpr bool pinsUnique() {
  constexpr int pins[] = {
      I2C_SDA, I2C_SCL, I2S_BCLK, I2S_LRCLK, I2S_DOUT, I2S_DIN,
      SPI_SCK, SPI_MISO, SPI_MOSI, LORA_CS, LORA_RST, LORA_DIO1,
      LORA_BUSY, SD_CS, GNSS_RX, GNSS_TX, BTN_PTT, BTN_SOS, BATTERY_ADC,
      BUZZER, LED_RGB, HAPTIC, LED_CHARGING, LED_TX, LED_RX,
      USB_D_MINUS, USB_D_PLUS, MAX2016_OUT_FWD, MAX2016_OUT_REF
  };
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    if (pins[i] < 0) continue;
    for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j)
      if (pins[i] == pins[j]) return false;
  }
  return true;
}
static_assert(pinsUnique(), "BoardConfig GPIO collision detected");

// ESP32-S3-WROOM-1-N16R8 reserves GPIO26..37 for package flash/PSRAM and
// GPIO0/3/45/46 are strapping pins. Keep the routing contract explicit so a
// future pin-map edit cannot silently consume a boot-critical or memory pin.
constexpr bool isForbiddenSharedPin(int pin) {
  return pin == 0 || pin == 3 || pin == 45 || pin == 46 ||
         (pin >= 26 && pin <= 37);
}
constexpr bool isRtcCapable(int pin) { return pin >= 0 && pin <= 21; }
constexpr bool isAdc1Pin(int pin) { return pin >= 1 && pin <= 10; }
constexpr bool isAdc2Pin(int pin) { return pin >= 11 && pin <= 20; }

static_assert(isAdc1Pin(BATTERY_ADC) && isAdc1Pin(MAX2016_OUT_FWD) &&
              isAdc1Pin(MAX2016_OUT_REF),
              "All onboard analogue sensors must remain on ADC1");
static_assert(isRtcCapable(LORA_DIO1) && isRtcCapable(BTN_PTT) &&
              isRtcCapable(BTN_SOS),
              "Deep-sleep wake sources must be RTC-capable GPIOs");
static_assert(!isForbiddenSharedPin(LORA_DIO1) &&
              !isForbiddenSharedPin(BTN_PTT) &&
              !isForbiddenSharedPin(BTN_SOS),
              "Wake GPIO must not use a strapping or flash/PSRAM pin");
static_assert(USB_D_MINUS == 19 && USB_D_PLUS == 20,
              "Native USB D-/D+ routing is fixed to GPIO19/GPIO20");
static_assert(isAdc2Pin(SPI_SCK) && isAdc2Pin(SPI_MISO) &&
              isAdc2Pin(SPI_MOSI),
              "Shared SPI pins are intentionally digital ADC2 GPIOs");
}
