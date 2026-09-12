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

// Auxiliary controls are assigned to currently-unused ESP32-S3-WROOM GPIOs.
// These nets MUST be routed on the PCB revision; they are not present in the
// supplied PCB mapping. Active-low buttons use internal pull-ups.
// GPIO47/48 are used only for auxiliary status I/O in this firmware.
constexpr int BTN_PTT = 21;
constexpr int BTN_SOS = 47;
constexpr int BATTERY_ADC = 1;
constexpr int STATUS_LED = 48;

// ESP32-S3 native USB uses GPIO19=D- and GPIO20=D+.
constexpr int USB_D_MINUS = 19;
constexpr int USB_D_PLUS = 20;

// WM8962 uses an external 24 MHz oscillator on the PCB.
// No ESP32 GPIO is assigned to MCLK.
constexpr uint8_t WM8962_I2C_ADDR = 0x1A;
constexpr uint32_t WM8962_MCLK_HZ = 24000000UL;
}
