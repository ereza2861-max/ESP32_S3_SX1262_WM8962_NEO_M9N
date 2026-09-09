#pragma once
#include <Arduino.h>

/*
 * PCB: ESP32-WROOM-32E + SX1276 + WM8960 + NEO-M8N + microSD
 *
 * Pin map is taken directly from the supplied KiCad pin-mapping package.
 * Do not use GPIO6..11 (ESP32 module flash).
 */
namespace Board {
constexpr int I2C_SDA = 21;
constexpr int I2C_SCL = 22;

// WM8960 I2S
// ESP32 -> WM8960: DOUT = codec DACDAT
// WM8960 -> ESP32: DIN  = codec ADCDAT
constexpr int I2S_BCLK = 32;
constexpr int I2S_LRCLK = 33;
constexpr int I2S_DOUT = 25;
constexpr int I2S_DIN  = 34;

// Shared SPI bus: SX1276 + microSD
constexpr int SPI_SCK  = 18;
constexpr int SPI_MISO = 19;
constexpr int SPI_MOSI = 23;

constexpr int LORA_CS   = 27; // SX1276 NSS
constexpr int LORA_RST  = 26;
constexpr int LORA_DIO0 = 35;
constexpr int LORA_DIO1 = 36; // optional, reserved

constexpr int SD_CS = 13;

// NEO-M8N UART
constexpr int GNSS_RX = 16; // ESP32 RX <- NEO-M8N TX
constexpr int GNSS_TX = 17; // ESP32 TX -> NEO-M8N RX
constexpr uint32_t GNSS_BAUD = 9600;

// The supplied PCB mapping has no dedicated button or battery ADC.
constexpr int BTN_PTT = -1;
constexpr int BTN_SOS = -1;
constexpr int BATTERY_ADC = -1;

// WM8960 uses an external 24 MHz oscillator on the PCB.
// No ESP32 GPIO is assigned to MCLK.
constexpr uint32_t WM8960_MCLK_HZ = 24000000UL;
}
