#pragma once
#include <Arduino.h>

/*
 * Target: ESP32-S3-WROOM-1 + SX1276 + WM8960 + NEO-M8N + microSD.
 *
 * IMPORTANT: this is a new S3 routing map. It is NOT electrically compatible
 * with the old ESP32-WROOM-32E PCB without PCB trace/net changes.
 * GPIO19/20 are reserved for the ESP32-S3 native USB D-/D+.
 */
namespace Board {
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;

// WM8960 I2S
// ESP32 -> WM8960: DOUT = codec DACDAT
// WM8960 -> ESP32: DIN  = codec ADCDAT
constexpr int I2S_BCLK = 4;
constexpr int I2S_LRCLK = 5;
constexpr int I2S_DOUT = 6;
constexpr int I2S_DIN  = 7;

// Shared SPI bus: SX1276 + microSD
constexpr int SPI_SCK  = 12;
constexpr int SPI_MISO = 13;
constexpr int SPI_MOSI = 11;

constexpr int LORA_CS   = 10; // SX1276 NSS
constexpr int LORA_RST  = 14;
constexpr int LORA_DIO0 = 2;
constexpr int LORA_DIO1 = 15; // optional, reserved

constexpr int SD_CS = 16;

// NEO-M8N UART
constexpr int GNSS_RX = 18; // ESP32-S3 RX <- NEO-M8N TX
constexpr int GNSS_TX = 17; // ESP32-S3 TX -> NEO-M8N RX
constexpr uint32_t GNSS_BAUD = 9600;

// The supplied PCB mapping has no dedicated button or battery ADC.
constexpr int BTN_PTT = -1;
constexpr int BTN_SOS = -1;
constexpr int BATTERY_ADC = -1;

// ESP32-S3 native USB uses GPIO19=D- and GPIO20=D+.
constexpr int USB_D_MINUS = 19;
constexpr int USB_D_PLUS = 20;

// WM8960 uses an external 24 MHz oscillator on the PCB.
// No ESP32 GPIO is assigned to MCLK.
constexpr uint32_t WM8960_MCLK_HZ = 24000000UL;
}
