#pragma once
#include <Arduino.h>

// Optional machine-local credentials. This file is intentionally ignored by Git.
#if __has_include("LocalConfig.h")
#include "LocalConfig.h"
#endif

namespace Config {
constexpr uint32_t SERIAL_BAUD = 115200;

#ifndef FIELDRADIO_AP_SSID
#define FIELDRADIO_AP_SSID "FieldRadio"
#endif
#ifndef FIELDRADIO_AP_PASSWORD
#define FIELDRADIO_AP_PASSWORD ""
#endif
#ifndef FIELDRADIO_WEB_USER
#define FIELDRADIO_WEB_USER ""
#endif
#ifndef FIELDRADIO_WEB_PASSWORD
#define FIELDRADIO_WEB_PASSWORD ""
#endif

constexpr char AP_SSID[] = FIELDRADIO_AP_SSID;
constexpr char AP_PASSWORD[] = FIELDRADIO_AP_PASSWORD;
constexpr char WEB_USER[] = FIELDRADIO_WEB_USER;
constexpr char WEB_PASSWORD[] = FIELDRADIO_WEB_PASSWORD;
#ifndef FIELDRADIO_LORA_KEY_HEX
#define FIELDRADIO_LORA_KEY_HEX ""
#endif
constexpr char LORA_KEY_HEX[] = FIELDRADIO_LORA_KEY_HEX;
// Credentials are supplied only by an ignored LocalConfig.h or build environment.
// An empty/default credential set keeps the network service disabled.
constexpr bool CREDENTIALS_CONFIGURED =
    AP_PASSWORD[0] != '\0' && WEB_USER[0] != '\0' && WEB_PASSWORD[0] != '\0';
constexpr uint16_t WEB_PORT = 80;

// SX1276 / legal regional setting must be changed to the frequency allowed
// by the local radio regulations and the actual RF matching network.
constexpr float LORA_FREQ_MHZ = 923.0f;
constexpr float LORA_BW_KHZ = 125.0f;
constexpr uint8_t LORA_SF = 7;
constexpr uint8_t LORA_CR = 5;
constexpr uint8_t LORA_SYNC_WORD = 0x12;
constexpr int8_t LORA_POWER_DBM = 14;
// Indonesia LPWAN nonseluler: 920-923 MHz, uplink duty cycle <= 1%.
// Keep the application inside this range unless a different regulatory
// profile is explicitly selected and validated for the deployment country.
constexpr float LORA_MIN_FREQ_MHZ = 920.0f;
constexpr float LORA_MAX_FREQ_MHZ = 923.0f;
constexpr uint32_t LORA_TX_TIMEOUT_MS = 5000;
constexpr uint32_t LORA_DUTY_WINDOW_MS = 3600000UL;
constexpr uint8_t LORA_DUTY_CYCLE_PERCENT = 1;
constexpr char LORA_DEFAULT_CALLSIGN[] = "FIELD";
constexpr uint16_t LORA_PREAMBLE = 8;
constexpr size_t LORA_MAX_PACKET = 220;
constexpr bool LORA_REQUIRE_ENCRYPTION = true;
// Listen-Before-Talk: CAD before every TX, with cooperative random backoff.
constexpr bool LORA_LBT_ENABLED = true;
constexpr uint8_t LORA_LBT_MAX_RETRIES = 5;
constexpr uint32_t LORA_LBT_BACKOFF_MIN_MS = 20;
constexpr uint32_t LORA_LBT_BACKOFF_MAX_MS = 100;
// Protocol v2 adds authenticated source ID and TTL/hop-limit metadata.
// v1 packets remain receivable, but cannot be safely forwarded because they
// do not carry forwarding metadata.
constexpr uint8_t LORA_PROTOCOL_VERSION = 2;
constexpr uint8_t LORA_LEGACY_PROTOCOL_VERSION = 1;
constexpr uint8_t LORA_INITIAL_TTL = 3;
constexpr size_t LORA_FORWARD_QUEUE_DEPTH = 6;
constexpr size_t LORA_DEDUP_CACHE_SIZE = 32;
constexpr uint32_t LORA_DEDUP_TTL_MS = 300000UL;
constexpr uint32_t LORA_FORWARD_RATE_LIMIT_MS = 1000UL;
constexpr uint8_t LORA_TAG_BYTES = 8;
constexpr int16_t VOICE_RSSI_THRESHOLD_DBM = -115;
constexpr int8_t VOICE_SNR_THRESHOLD_DB = -12;
constexpr uint32_t RX_ACTIVITY_HOLD_MS = 250;
constexpr uint32_t WEB_RATE_LIMIT_MS = 500;
constexpr uint32_t SOS_RATE_LIMIT_MS = 3000;

// WM8960/ESP32-S3 I2S audio
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint8_t AUDIO_BITS = 16;
constexpr uint8_t AUDIO_CHANNELS = 2;
constexpr uint8_t AUDIO_SOURCE_WM8960_MIC = 0;
constexpr uint8_t AUDIO_SOURCE_LINEIN2 = 1;
constexpr uint8_t AUDIO_SOURCE_LINEIN3 = 2;
constexpr uint8_t AUDIO_SOURCE_USB = 3;
constexpr size_t USB_RECORD_BUFFER_BYTES = 32768;
constexpr size_t USB_MIC_BUFFER_BYTES = 16384;
constexpr size_t USB_AEC_REFERENCE_BYTES = 65536;
constexpr bool AEC_ENABLED_BY_DEFAULT = true;
constexpr uint32_t AEC_SAMPLE_RATE = 16000;
constexpr uint16_t AEC_FRAME_SAMPLES = 512; // 32 ms @ 16 kHz
constexpr uint8_t AEC_FILTER_LENGTH = 4;
constexpr uint8_t USB_DYNAMIC_RATE_MIN = 16000 / 1000;
constexpr uint32_t RECORD_MAX_SECONDS = 300;
constexpr uint16_t I2S_DMA_BUF_COUNT = 8;
constexpr uint16_t I2S_DMA_BUF_LEN = 256;

// Battery monitoring. BATTERY_ADC stays disabled on the supplied PCB (-1).
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;
constexpr float BATTERY_LOW_THRESHOLD = 3.4f;
constexpr float BATTERY_CRITICAL = 3.2f;
constexpr uint32_t BATTERY_SAMPLE_PERIOD_MS = 10000;
constexpr uint32_t CRITICAL_SHUTDOWN_DELAY_MS = 1500;
constexpr bool DEEP_SLEEP_ENABLED = false;
constexpr uint32_t DEEP_SLEEP_IDLE_MS = 300000;

// Watchdog.
constexpr uint32_t TASK_WDT_TIMEOUT_MS = 10000;

// Services
constexpr uint32_t GPS_REPORT_PERIOD_MS = 30000;
constexpr uint32_t STATUS_PERIOD_MS = 1000;
constexpr uint32_t SOS_REPEAT_MS = 5000;
constexpr uint32_t GNSS_STALE_MS = 10000;
constexpr size_t MAX_WEB_BODY = 4096;
constexpr size_t MAX_WEB_FILES = 128;
constexpr size_t MAX_PATH = 96;
constexpr size_t AUDIO_IO_BYTES = 1024;
constexpr size_t PLAYBACK_PREBUFFER_BYTES = 16384;
constexpr size_t USB_TRANSPORT_BUFFER_BYTES = 16384;
constexpr uint8_t PLAYBACK_QUEUE_DEPTH = 16;
constexpr uint32_t RECORD_SPLIT_SECONDS = 300;
constexpr uint32_t RECORD_MIN_FREE_BYTES = 2UL * 1024UL * 1024UL;
constexpr uint32_t RECORD_MAX_TOTAL_BYTES = 512UL * 1024UL * 1024UL;
constexpr uint32_t TRACK_LOG_PERIOD_MS = 10000;
constexpr uint32_t WIFI_AP_IDLE_TIMEOUT_MS = 600000UL;
constexpr uint32_t WIFI_AP_RETRY_MS = 30000UL;
constexpr float VOX_THRESHOLD = 0.08f;
constexpr uint32_t VOX_HANG_MS = 700;
constexpr uint32_t VOICE_FRAME_MS = 20;
constexpr uint32_t AUDIO_TONE_MAX_MS = 2000;
constexpr uint32_t AUDIO_VU_HOLD_MS = 250;

// USB Audio Class device
constexpr char USB_AUDIO_NAME[] = "FieldRadio USB Audio";
}
