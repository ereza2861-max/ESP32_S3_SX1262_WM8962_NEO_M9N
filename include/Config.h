#pragma once
#include <Arduino.h>

namespace Config {
constexpr uint32_t SERIAL_BAUD = 115200;

constexpr char AP_SSID[] = "FieldRadio";
constexpr char AP_PASSWORD[] = "ChangeMe-1234";
constexpr char WEB_USER[] = "admin";
constexpr char WEB_PASSWORD[] = "ChangeMe-Web-1234";
constexpr uint16_t WEB_PORT = 80;

// SX1276 / legal regional setting must be changed to the frequency allowed
// by the local radio regulations and the actual RF matching network.
constexpr float LORA_FREQ_MHZ = 923.0f;
constexpr float LORA_BW_KHZ = 125.0f;
constexpr uint8_t LORA_SF = 7;
constexpr uint8_t LORA_CR = 5;
constexpr uint8_t LORA_SYNC_WORD = 0x12;
constexpr int8_t LORA_POWER_DBM = 14;
constexpr uint16_t LORA_PREAMBLE = 8;
constexpr size_t LORA_MAX_PACKET = 220;

// WM8960/ESP32 I2S audio
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint8_t AUDIO_BITS = 16;
constexpr uint8_t AUDIO_CHANNELS = 2;
constexpr uint32_t RECORD_MAX_SECONDS = 300;
constexpr uint16_t I2S_DMA_BUF_COUNT = 8;
constexpr uint16_t I2S_DMA_BUF_LEN = 256;

// Services
constexpr uint32_t GPS_REPORT_PERIOD_MS = 30000;
constexpr uint32_t STATUS_PERIOD_MS = 1000;
constexpr uint32_t SOS_REPEAT_MS = 5000;
constexpr uint32_t GNSS_STALE_MS = 10000;
constexpr size_t MAX_WEB_BODY = 4096;
constexpr size_t MAX_PATH = 96;
constexpr size_t AUDIO_IO_BYTES = 1024;

// Web/OTA
constexpr bool OTA_ENABLED = false;
constexpr uint32_t OTA_TIMEOUT_MS = 120000;

// A2DP sink name
constexpr char BT_NAME[] = "FieldRadio Audio";
}
