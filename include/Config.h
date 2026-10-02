#pragma once
#include <Arduino.h>
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#ifndef CONFIG_SECURE_BOOT_V2_ENABLED
#define CONFIG_SECURE_BOOT_V2_ENABLED 0
#endif

// Optional machine-local credentials. This file is intentionally ignored by Git.
#if __has_include("LocalConfig.h")
#include "LocalConfig.h"
#endif

namespace Config {
constexpr uint32_t SERIAL_BAUD = 115200;

constexpr bool mqttTlsIsMandatory(bool productionBuild, bool secureBootEnabled) {
  return productionBuild || secureBootEnabled;
}

constexpr bool mqttTlsIsMandatory() {
#if defined(FIELDRADIO_PRODUCTION_BUILD)
  constexpr bool productionBuild = true;
#else
  constexpr bool productionBuild = false;
#endif
#if defined(CONFIG_SECURE_BOOT_V2_ENABLED) && CONFIG_SECURE_BOOT_V2_ENABLED
  constexpr bool secureBootEnabled = true;
#else
  constexpr bool secureBootEnabled = false;
#endif
  return mqttTlsIsMandatory(productionBuild, secureBootEnabled);
}


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
constexpr uint16_t WEB_PORT = 443;

// SX1262 / legal regional setting must be changed to the frequency allowed
// by the local radio regulations and the actual RF matching network.
#ifndef FIELDRADIO_LORA_FREQ_MHZ
#define FIELDRADIO_LORA_FREQ_MHZ 923.0f
#endif
#ifndef FIELDRADIO_LORA_BW_KHZ
#define FIELDRADIO_LORA_BW_KHZ 125.0f
#endif
#ifndef FIELDRADIO_LORA_SF
#define FIELDRADIO_LORA_SF 7
#endif
#ifndef FIELDRADIO_LORA_CR
#define FIELDRADIO_LORA_CR 5
#endif
#ifndef FIELDRADIO_LORA_POWER_DBM
#define FIELDRADIO_LORA_POWER_DBM 14
#endif
#ifndef FIELDRADIO_LORA_SYNC_WORD
#define FIELDRADIO_LORA_SYNC_WORD 0x12
#endif
constexpr float LORA_FREQ_MHZ = FIELDRADIO_LORA_FREQ_MHZ;
constexpr float LORA_BW_KHZ = FIELDRADIO_LORA_BW_KHZ;
constexpr uint8_t LORA_SF = FIELDRADIO_LORA_SF;
constexpr uint8_t LORA_CR = FIELDRADIO_LORA_CR;
constexpr int8_t LORA_POWER_DBM = FIELDRADIO_LORA_POWER_DBM;
constexpr uint8_t LORA_SYNC_WORD = FIELDRADIO_LORA_SYNC_WORD;
// 0 V selects the SX1262 crystal/XTAL path in RadioLib. Change only if the
// actual PCB routes a TCXO to the radio reference input.
constexpr float LORA_TCXO_VOLTAGE = 0.0f;
// Indonesia LPWAN nonseluler: 920-923 MHz, uplink duty cycle <= 1%.
// Keep the application inside this range unless a different regulatory
// profile is explicitly selected and validated for the deployment country.
constexpr float LORA_MIN_FREQ_MHZ = 920.0f;
constexpr float LORA_MAX_FREQ_MHZ = 923.0f;
constexpr uint32_t LORA_TX_TIMEOUT_MS = 5000;
constexpr uint32_t LORA_DUTY_WINDOW_MS = 3600000UL;
constexpr uint8_t LORA_DUTY_CYCLE_PERCENT = 1;
constexpr char LORA_DEFAULT_CALLSIGN[] = "FIELD";
// Deep-sleep RX uses SX1262 receive duty-cycle mode. A longer preamble is
// required so a duty-cycled receiver can reliably acquire the packet.
constexpr uint16_t LORA_PREAMBLE = 32;
constexpr bool LORA_RX_DUTY_CYCLE_ENABLED = true;
constexpr uint16_t LORA_RX_DUTY_MIN_SYMBOLS = 8;
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
constexpr uint8_t LORA_TYPE_TEXT = 0;
constexpr uint8_t LORA_TYPE_VOICE = 1;
constexpr uint8_t LORA_TYPE_SOS = 2;
constexpr uint8_t LORA_TYPE_SOS_ACK = 3;
constexpr uint8_t LORA_TYPE_TEXT_ACK = 4;
constexpr uint8_t LORA_TYPE_VOICE_ACK = 5;
constexpr uint8_t LORA_TYPE_NEIGHBOR_BEACON = 6;
constexpr uint8_t LORA_LEGACY_PROTOCOL_VERSION = 1;
constexpr uint8_t LORA_INITIAL_TTL = 3;
constexpr size_t LORA_FORWARD_QUEUE_DEPTH = 6;
constexpr size_t LORA_DEDUP_CACHE_SIZE = 32;
constexpr size_t LORA_REPLAY_SOURCE_CACHE_SIZE = 32;
constexpr uint8_t LORA_REPLAY_WINDOW_BITS = 32;
constexpr uint32_t LORA_TX_SEQUENCE_RESERVATION = 256;
constexpr uint32_t LORA_DEDUP_TTL_MS = 300000UL;
constexpr uint32_t LORA_FORWARD_RATE_LIMIT_MS = 1000UL;
constexpr uint32_t NEIGHBOR_TTL_MS = 240000UL;
constexpr uint32_t LORA_NEIGHBOR_BEACON_PERIOD_MS = 30000UL;
constexpr uint8_t LORA_VOICE_WINDOW_SIZE = 8;
constexpr uint32_t LORA_VOICE_ACK_TIMEOUT_MS = 140UL;
constexpr uint32_t LORA_VOICE_ACK_WEAK_TIMEOUT_MS = 300UL;
constexpr uint8_t LORA_VOICE_MAX_RETRIES = 3;
constexpr size_t LORA_TX_QUEUE_DEPTH = 12;
constexpr size_t LORA_STORE_FORWARD_MAX_RECORDS = 12;
constexpr size_t LORA_FRAGMENT_MAX_BYTES = 2048;
constexpr uint8_t LORA_FRAGMENT_VERSION = 1;
constexpr uint8_t LORA_FRAGMENT_HEADER_BYTES = 10;
constexpr uint8_t LORA_FRAGMENT_MAX_COUNT = 16;
constexpr uint8_t LORA_FRAGMENT_MAX_RETRIES = 3;
constexpr uint32_t LORA_FRAGMENT_REASSEMBLY_TIMEOUT_MS = 30000UL;
constexpr uint32_t LORA_STORE_FORWARD_MAX_BYTES = 32UL * 1024UL;
constexpr uint32_t LORA_RETRY_BASE_MS = 100UL;
constexpr uint32_t LORA_FORWARD_VOICE_RATE_LIMIT_MS = 100UL;
constexpr size_t LORA_FORWARD_SOURCE_CACHE_SIZE = 16;
constexpr size_t VOICE_REORDER_BUFFER_SIZE = 8;
constexpr uint32_t VOICE_REORDER_HOLD_MS = 80;
constexpr size_t MESSAGE_HISTORY_SIZE = 32;
constexpr size_t LORA_PACKET_LOG_SIZE = 64;
constexpr size_t HEALTH_LOG_SIZE = 16;
constexpr uint32_t TRACK_MAX_BYTES = 1024UL * 1024UL;
constexpr uint8_t TRACK_ROTATIONS = 3;
constexpr size_t WEB_UPLOAD_MAX_BYTES = 8UL * 1024UL * 1024UL;
constexpr uint32_t USB_VOLUME_PERSIST_DELAY_MS = 1500UL;
constexpr uint8_t LORA_TAG_BYTES = 16;
// Optional frequency-hopping profile. Channel indexes map linearly across the
// configured legal band; deployments can change the spacing without changing
// the packet format.
constexpr uint8_t HOP_CHANNEL_MAX = 8;
constexpr float HOP_CHANNEL_FREQ_MHZ[HOP_CHANNEL_MAX] = {
    920.2f, 920.6f, 921.0f, 921.4f,
    921.8f, 922.2f, 922.6f, 923.0f};
constexpr uint32_t HOP_DWELL_MS = 1000UL;
static_assert(HOP_CHANNEL_MAX == 0 ||
              HOP_CHANNEL_FREQ_MHZ[HOP_CHANNEL_MAX - 1U] <= LORA_MAX_FREQ_MHZ,
              "Rev-C hop channel profile exceeds configured legal LoRa band");
constexpr uint8_t HOP_LEGACY_RX_EVERY = 3;
constexpr uint32_t SCANNER_DEFAULT_DWELL_MS = 100;
constexpr uint32_t SCANNER_MIN_DWELL_MS = 25;
constexpr uint32_t SCANNER_MAX_DWELL_MS = 1000;
constexpr size_t SCANNER_MAX_CHANNELS = HOP_CHANNEL_MAX;
constexpr int16_t VOICE_RSSI_THRESHOLD_DBM = -115;
constexpr int8_t VOICE_SNR_THRESHOLD_DB = -12;
constexpr uint32_t RX_ACTIVITY_HOLD_MS = 250;
constexpr int16_t LORA_JAM_RSSI_THRESHOLD_DBM = -80;
constexpr uint8_t LORA_JAM_OCCUPANCY_THRESHOLD_PERCENT = 70;
constexpr uint32_t WEB_RATE_LIMIT_MS = 500;

// Shared UI/backend configuration range contract. Keep parsing and runtime
// validation on the same constants so boundary behavior cannot drift.
constexpr uint32_t WAKE_PERIOD_SEC_MIN = 60UL;
constexpr uint32_t WAKE_PERIOD_SEC_MAX = 7UL * 24UL * 60UL * 60UL;
constexpr uint32_t DEEP_SLEEP_IDLE_MS_MIN = 60000UL;
constexpr uint32_t DEEP_SLEEP_IDLE_MS_MAX = 24UL * 60UL * 60UL * 1000UL;
constexpr float BATTERY_CRITICAL_THRESHOLD_MIN = 2.5f;
constexpr float BATTERY_LOW_THRESHOLD_MAX = 4.2f;
constexpr uint32_t MQTT_RECONNECT_MS_MIN = 1000UL;
constexpr uint32_t MQTT_RECONNECT_MS_MAX = 3600000UL;
constexpr uint32_t MQTT_TELEMETRY_PERIOD_MS_MIN = 1000UL;
constexpr uint32_t MQTT_TELEMETRY_PERIOD_MS_MAX = 86400000UL;
constexpr uint32_t BLE_SCAN_INTERVAL_MS_MIN = 100UL;
constexpr uint32_t BLE_SCAN_INTERVAL_MS_MAX = 60000UL;
constexpr uint32_t WEB_SESSION_TIMEOUT_MS_MIN = 60000UL;
constexpr uint32_t WEB_SESSION_TIMEOUT_MS_MAX = 86400000UL;
constexpr uint32_t WEB_AUTH_RATE_LIMIT_MS_MIN = 100UL;
constexpr uint32_t WEB_AUTH_RATE_LIMIT_MS_MAX = 600000UL;
constexpr uint8_t LORA_REPLAY_WINDOW_BITS_MIN = 8;
constexpr uint8_t CERT_RENEWAL_THRESHOLD_DAYS_MIN = 1;
constexpr uint16_t CERT_RENEWAL_THRESHOLD_DAYS_MAX = 3650;
constexpr uint32_t WEB_POST_CSRF_TOKEN_BYTES = 16;
constexpr uint32_t LORA_REKEY_PERIOD_SEC = 86400UL;
constexpr uint32_t LORA_REPLAY_TIME_WINDOW_SEC = 300UL;
// ECDH support is always compiled. RuntimeConfig::ecdhRekeyPolicy is the
// only activation switch: 0 = legacy AES-GCM framing, 1 = ECDH rekey framing.
// A build flag cannot disable compilation of the ECDH implementation.
#ifdef FIELDRADIO_LORA_ECDH_REKEY_ENABLED
#undef FIELDRADIO_LORA_ECDH_REKEY_ENABLED
#endif
#define FIELDRADIO_LORA_ECDH_REKEY_ENABLED 1
constexpr bool LORA_ECDH_REKEY_ENABLED = true;
constexpr uint32_t LORA_ECDH_KEY_RETENTION_SEC =
    2UL * LORA_REKEY_PERIOD_SEC;
constexpr uint8_t LORA_ECDH_PROTOCOL_VERSION = 1;
constexpr uint8_t LORA_PROTOCOL_VERSION_ECDH = 5;
constexpr uint8_t LORA_ECDH_KEY_EPOCH_DELTA_CURRENT = 0;
constexpr uint8_t LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS = 1;
constexpr size_t LORA_ECDH_V5_HEADER_BYTES = 20;
constexpr uint8_t LORA_ECDH_BEACON_MAGIC = 0xE2;
constexpr size_t LORA_ECDH_PUBLIC_KEY_BYTES = 32;
constexpr size_t LORA_ECDH_BEACON_BYTES =
    1U + 1U + sizeof(uint32_t) +
    LORA_ECDH_PUBLIC_KEY_BYTES + LORA_ECDH_PUBLIC_KEY_BYTES;

// Replay persistence backend. MRAM is authoritative once detected; NVS is
// retained only as the pre-authority fallback/migration source.
#ifndef FIELDRADIO_REPLAY_BACKEND
#define FIELDRADIO_REPLAY_BACKEND 0
#endif
enum class ReplayStoreBackend : uint8_t {
  BACKEND_MRAM = 0,
  BACKEND_NVS_JOURNAL = 1,
};
constexpr ReplayStoreBackend REPLAY_STORE_BACKEND =
    static_cast<ReplayStoreBackend>(FIELDRADIO_REPLAY_BACKEND);
static_assert(FIELDRADIO_REPLAY_BACKEND == 0 || FIELDRADIO_REPLAY_BACKEND == 1,
              "FIELDRADIO_REPLAY_BACKEND must be 0 (MRAM) or 1 (NVS journal)");
constexpr uint8_t REPLAY_STORE_VERSION = 1;
constexpr uint32_t MRAM_SIZE_BYTES = 32768UL;
constexpr uint16_t REPLAY_MRAM_HEADER_ADDR = 0x0000;
constexpr uint16_t REPLAY_MRAM_HEADER_BYTES = 16;
constexpr uint16_t REPLAY_MRAM_BANK0_ADDR = 0x0010;
constexpr uint16_t REPLAY_MRAM_BANK1_ADDR = 0x0410;
constexpr uint16_t REPLAY_MRAM_SLOT_BYTES = 32;
constexpr uint8_t REPLAY_MRAM_BANK_COUNT = 2;
constexpr uint16_t PERSISTENT_CONFIG_MRAM_SLOT_A = 0x1000;
constexpr uint16_t PERSISTENT_CONFIG_MRAM_SLOT_B = 0x2000;
constexpr uint16_t PERSISTENT_CONFIG_MRAM_COMMIT_A = 0x3000;
constexpr uint16_t PERSISTENT_CONFIG_MRAM_COMMIT_B = 0x3001;
constexpr uint16_t PERSISTENT_CONFIG_MRAM_MARKER = 0x3010;
constexpr uint16_t PERSISTENT_CONFIG_MRAM_MARKER_BYTES = 16;
constexpr uint16_t PERSISTENT_CONFIG_MRAM_SLOT_BYTES = 4096;
constexpr uint8_t PERSISTENT_CONFIG_MRAM_COMMIT = 0xA5;
constexpr size_t REPLAY_NVS_JOURNAL_RECORDS = 64;
constexpr uint8_t REPLAY_NVS_COMPACT_PERCENT = 75;

// MAX2016 RSSI-mode transfer characteristics at 0.9 GHz. These are typical
// datasheet values for R1=R2=0 ohm; verify against the assembled RF path in
// the lab and replace with measured slope/intercept if needed.
constexpr float MAX2016_SLOPE_MV_PER_DB = 18.1f;
constexpr float MAX2016_INTERCEPT_DBM = -97.0f;
constexpr float MAX2016_MIN_DBM = -70.0f;
constexpr float MAX2016_MAX_DBM = 10.0f;
constexpr adc_attenuation_t MAX2016_ADC_ATTENUATION = ADC_11db;
constexpr float MAX2016_EMA_ALPHA = 0.25f;
constexpr float MAX2016_VSWR_MAX = 99.0f;
constexpr float MAX2016_ANTENNA_OK_VSWR = 3.0f;
constexpr uint32_t MAX2016_READ_DELAY_US = 0;
constexpr uint32_t CPU_ACTIVE_MHZ = 240;
constexpr uint32_t CPU_IDLE_MHZ = 80;
constexpr uint32_t WEB_SESSION_TIMEOUT_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t WEB_AUTH_LOG_ROTATE_BYTES = 64UL * 1024UL;
constexpr uint32_t CONFIG_AUDIT_LOG_ROTATE_BYTES = 64UL * 1024UL;
constexpr uint32_t AUDIO_LOG_ROTATE_BYTES = 64UL * 1024UL;
constexpr uint32_t LORA_LOG_ROTATE_BYTES = 64UL * 1024UL;
constexpr uint32_t HEALTH_LOG_ROTATE_BYTES = 64UL * 1024UL;
constexpr uint32_t LOG_PERSIST_PERIOD_MS = 5000UL;
constexpr uint32_t RANGE_TEST_PERIOD_MS = 5000UL;
constexpr uint8_t LORA_RANGE_TEST_MAGIC = 0xD4;
constexpr uint8_t LORA_RANGE_TEST_ACK_MAGIC = 0xD5;
constexpr uint8_t LORA_RANGE_TEST_VERSION = 1;
constexpr uint32_t RANGE_TEST_MAX_DURATION_MS = 30UL * 60UL * 1000UL;
constexpr uint8_t CONFIG_VERSION = 11;
constexpr uint16_t ATOMIC_CONFIG_SCHEMA_VERSION = 3;
constexpr uint32_t SOS_RATE_LIMIT_MS = 3000;
constexpr uint8_t SOS_MAX_RETRIES = 3;
constexpr uint32_t SOS_ESCALATION_DELAY_MS = 30000UL;
constexpr uint32_t SOS_BEACON_PERIOD_MS = 15000UL;
constexpr uint32_t MESSAGE_HISTORY_ROTATE_BYTES = 64UL * 1024UL;
constexpr uint32_t CAPTURE_MAX_DURATION_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t ADR_REEVALUATE_MS = 10000UL;

// LoRaWAN Class A regional configuration. RegionalProfile indexes match the
// RadioLib LoRaWANBand_t variants AS923, AS923_2, AS923_3 and AS923_4.
constexpr uint8_t LORAWAN_REGION_DEFAULT = 1; // AS923_2
constexpr uint16_t LORAWAN_UPLINK_PERIOD_SEC_DEFAULT = 300;
constexpr uint8_t LORAWAN_DEFAULT_FPORT = 1;
constexpr uint32_t LORAWAN_JOIN_RETRY_MIN_MS = 30000UL;
constexpr uint32_t LORAWAN_JOIN_RETRY_MAX_MS = 900000UL;
constexpr uint8_t LORAWAN_JOIN_BACKOFF_MULT = 2;
constexpr size_t LORAWAN_MAX_PAYLOAD = 51;
constexpr size_t LORAWAN_MAX_DOWNLINK = 242;
constexpr size_t LORAWAN_DOWNLINK_QUEUE = 4;
constexpr uint32_t LORAWAN_DOWNLINK_HOLD_MS = 60000UL;
constexpr bool LORAWAN_DUTY_CYCLE_ENABLED = true;
constexpr bool LORAWAN_DWELL_TIME_ENABLED = false;
constexpr uint16_t LORAWAN_MAX_DWELL_MS = 400;
constexpr uint32_t LORAWAN_RX2_FREQ_HZ_AS923_2 = 921400000UL;
constexpr uint8_t LORAWAN_RX2_DR_AS923_2 = 2;
constexpr bool LORA_USE_AES_GCM = true;
constexpr uint8_t LORA_PROTOCOL_VERSION_GCM = 4;
constexpr uint8_t LORA_TYPE_FRAG_DATA = 7;
constexpr uint8_t LORA_TYPE_FRAG_ACK = 8;
constexpr uint8_t LORA_TYPE_SENSOR_TELEMETRY = 9;
constexpr uint32_t SENSOR_REPORT_PERIOD_MS = 60000UL;
constexpr float SENSOR_REPORT_DELTA_THRESHOLD = 0.02f;
constexpr size_t SENSOR_LORA_MAX_PAYLOAD = 40;
constexpr size_t SENSOR_LORA_QUEUE_DEPTH = 16;
constexpr uint8_t LORA_FRAGMENT_WINDOW_SIZE = 8;
constexpr uint32_t LORA_FRAGMENT_ACK_TIMEOUT_MS = 1500UL;


// WM8962/ESP32-S3 I2S audio
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint8_t AUDIO_BITS = 16;
constexpr uint8_t AUDIO_CHANNELS = 2;
constexpr uint8_t AUDIO_SOURCE_WM8962_MIC = 0;
constexpr uint8_t AUDIO_SOURCE_LINEIN2 = 1;
constexpr uint8_t AUDIO_SOURCE_LINEIN3 = 2;
constexpr uint8_t AUDIO_SOURCE_USB = 3;

// WM8962 Class-D speaker contract. These are compile-time hardware gates:
// SPKVDD itself is supplied by the PCB and is not software-programmable.
// The WM8962 Class-D output is BTL only: stereo mode is specified for 8 ohm
// loads, while mono mode is specified for a 4 ohm load. "Single-ended" is
// intentionally rejected when Class-D is enabled.
enum class ClassDOutputMode : uint8_t {
  BTL = 0,
  SingleEnded = 1,
};
constexpr bool CLASS_D_ENABLED = false;
// Hardware evidence remains a separate immutable gate until schematic,
// netlist, PCB routing, and speaker/load evidence are available.
constexpr bool CLASS_D_HARDWARE_EVIDENCE_AVAILABLE = false;
static_assert(!CLASS_D_ENABLED || CLASS_D_HARDWARE_EVIDENCE_AVAILABLE,
              "CLASS_D_ENABLED requires hardware evidence");
constexpr ClassDOutputMode CLASS_D_OUTPUT_MODE = ClassDOutputMode::BTL;
constexpr bool CLASS_D_MONO = false;
constexpr uint8_t CLASS_D_SPEAKER_IMPEDANCE_OHMS = 8;
constexpr uint16_t CLASS_D_EXPECTED_SPKVDD_MV = 5000;
constexpr uint8_t CLASS_D_BOOST_LEVEL = 0; // 0..7 => 0,1.5,...,12 dB
constexpr uint16_t CLASS_D_MAX_SPKVDD_CURRENT_MA = 0; // 0 = not specified/limited by firmware

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

// Battery monitoring. BATTERY_ADC is a routed Rev-C ADC1 input; keep this
// comment synchronized with BoardConfig.h and docs/PCB_MAPPING.md.
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;
constexpr float BATTERY_LOW_THRESHOLD = 3.4f;
constexpr float BATTERY_CRITICAL = 3.2f;
constexpr uint32_t BATTERY_SAMPLE_PERIOD_MS = 10000;
constexpr uint32_t BATTERY_GAUGE_POLL_MS = 10000;
constexpr uint16_t I2C_TIMEOUT_MS = 50;
constexpr uint32_t GNSS_TIME_SYNC_PERIOD_MS = 12UL * 60UL * 60UL * 1000UL;
constexpr uint32_t GNSS_PPS_VALID_US = 2000000UL;
constexpr uint32_t BATTERY_HEALTH_PERSIST_MS = 60000;
constexpr float BATTERY_FULL_V = 4.15f;
constexpr float BATTERY_RECHARGE_START_V = 4.05f;
constexpr float BATTERY_CYCLE_RESET_V = 3.30f;
constexpr uint8_t BATTERY_HEALTH_VERSION = 1;
constexpr float BATTERY_TX_POWER_LOW_DBM = 10.0f;
constexpr float BATTERY_TX_POWER_CRITICAL_DBM = 6.0f;
constexpr float BATTERY_PERCENT_FULL_V = 4.20f;
constexpr float BATTERY_PERCENT_EMPTY_V = 3.20f;
constexpr uint32_t CRITICAL_SHUTDOWN_DELAY_MS = 1500;
constexpr bool DEEP_SLEEP_ENABLED = true;
constexpr uint32_t DEEP_SLEEP_WAKE_GRACE_MS = 5000;
constexpr uint32_t DEEP_SLEEP_IDLE_MS = 300000;

// Watchdog.
constexpr uint32_t TASK_WDT_TIMEOUT_MS = 10000;

// BLE sensor-reader parameters. These are compile-time build parameters.
// The external ESP32-C3 sensor node advertises the custom GATT service and
// implements the v1 contract in shared/SensorProtocol.h.
#ifndef SENSOR_READER_ENABLED
#define SENSOR_READER_ENABLED 1
#endif
#ifndef SENSOR_TRANSPORT
#define SENSOR_TRANSPORT 0
#endif
#ifndef SENSOR_MAX_NODES
#define SENSOR_MAX_NODES 2
#endif
#ifndef SENSOR_MAX_SENSORS_PER_NODE
// Gateway capacity matches the largest existing Sensor Node profile (12).
#define SENSOR_MAX_SENSORS_PER_NODE 12
#endif
#ifndef SENSOR_SCAN_INTERVAL_MS
#define SENSOR_SCAN_INTERVAL_MS 5000UL
#endif
#ifndef SENSOR_SCAN_WINDOW_MS
#define SENSOR_SCAN_WINDOW_MS 80U
#endif
#ifndef SENSOR_SCAN_DURATION_MS
#define SENSOR_SCAN_DURATION_MS 1500UL
#endif
#ifndef SENSOR_CONNECT_TIMEOUT_MS
#define SENSOR_CONNECT_TIMEOUT_MS 5000UL
#endif
#ifndef SENSOR_TASK_PERIOD_MS
#define SENSOR_TASK_PERIOD_MS 500UL
#endif
#ifndef SENSOR_NODE_EVICTION_MS
#define SENSOR_NODE_EVICTION_MS 600000UL
#endif
#ifndef SENSOR_MTU
#define SENSOR_MTU 128U
#endif
#ifndef SENSOR_ACTIVE_SCAN
#define SENSOR_ACTIVE_SCAN 1
#endif
#ifndef SENSOR_REQUIRE_ENCRYPTION
#define SENSOR_REQUIRE_ENCRYPTION 1
#endif
#ifndef BLE_PAIRING_ENABLED
#define BLE_PAIRING_ENABLED 1
#endif
#ifndef BLE_PAIRING_PASSKEY_DERIVATION_LABEL
#define BLE_PAIRING_PASSKEY_DERIVATION_LABEL "FieldRadio-BLE-Pair-v1"
#endif
#ifndef BLE_PAIRING_MAX_FAILURES
#define BLE_PAIRING_MAX_FAILURES 3
#endif
constexpr uint8_t BLE_MAX_BONDS = 8;
#ifndef BLE_PAIRING_BLOCK_MS
#define BLE_PAIRING_BLOCK_MS 60000UL
#endif
#ifndef SENSOR_KEEP_AWAKE
#define SENSOR_KEEP_AWAKE 1
#endif

enum class SensorTransport : uint8_t {
  BLE_NIMBLE_GATT = 0,
};
constexpr bool SENSOR_READER_ENABLED_VALUE = SENSOR_READER_ENABLED != 0;
constexpr SensorTransport SENSOR_TRANSPORT_VALUE =
    static_cast<SensorTransport>(SENSOR_TRANSPORT);
constexpr size_t SENSOR_MAX_NODES_VALUE = SENSOR_MAX_NODES;
constexpr size_t SENSOR_MAX_SENSORS_PER_NODE_VALUE = SENSOR_MAX_SENSORS_PER_NODE;
constexpr uint32_t SENSOR_SCAN_INTERVAL_MS_VALUE = SENSOR_SCAN_INTERVAL_MS;
constexpr uint16_t SENSOR_SCAN_WINDOW_MS_VALUE = SENSOR_SCAN_WINDOW_MS;
constexpr uint32_t SENSOR_SCAN_DURATION_MS_VALUE = SENSOR_SCAN_DURATION_MS;
constexpr uint32_t SENSOR_CONNECT_TIMEOUT_MS_VALUE = SENSOR_CONNECT_TIMEOUT_MS;
constexpr uint32_t SENSOR_TASK_PERIOD_MS_VALUE = SENSOR_TASK_PERIOD_MS;
constexpr uint32_t SENSOR_NODE_EVICTION_MS_VALUE = SENSOR_NODE_EVICTION_MS;
constexpr uint16_t SENSOR_MTU_VALUE = SENSOR_MTU;
constexpr bool SENSOR_ACTIVE_SCAN_VALUE = SENSOR_ACTIVE_SCAN != 0;
constexpr bool SENSOR_REQUIRE_ENCRYPTION_VALUE = SENSOR_REQUIRE_ENCRYPTION != 0;
constexpr bool BLE_PAIRING_ENABLED_VALUE = BLE_PAIRING_ENABLED != 0;
constexpr uint8_t BLE_PAIRING_MAX_FAILURES_VALUE = BLE_PAIRING_MAX_FAILURES;
constexpr uint32_t BLE_PAIRING_BLOCK_MS_VALUE = BLE_PAIRING_BLOCK_MS;
constexpr bool SENSOR_KEEP_AWAKE_VALUE = SENSOR_KEEP_AWAKE != 0;
static_assert(SENSOR_TRANSPORT == 0,
              "SENSOR_TRANSPORT currently supports only NimBLE GATT (0)");
static_assert(SENSOR_MAX_NODES >= 1 && SENSOR_MAX_NODES <= 3,
              "SENSOR_MAX_NODES must be 1..3 with the default NimBLE connection budget");
static_assert(SENSOR_MAX_SENSORS_PER_NODE >= 1 && SENSOR_MAX_SENSORS_PER_NODE <= 16,
              "SENSOR_MAX_SENSORS_PER_NODE must be 1..16");
// Queue depth is intentionally unchanged; it already covers a full 12-sensor node burst.
static_assert(SENSOR_LORA_QUEUE_DEPTH >= SENSOR_MAX_SENSORS_PER_NODE,
              "SENSOR_LORA_QUEUE_DEPTH must cover one full sensor roster");
static_assert(SENSOR_SCAN_WINDOW_MS > 0 && SENSOR_SCAN_WINDOW_MS <= SENSOR_SCAN_INTERVAL_MS,
              "SENSOR_SCAN_WINDOW_MS must be <= SENSOR_SCAN_INTERVAL_MS");
static_assert(SENSOR_MTU >= 23 && SENSOR_MTU <= 247,
              "SENSOR_MTU must be 23..247 for the v1 GATT contract");
static_assert(BLE_MAX_BONDS >= 1 && BLE_MAX_BONDS <= 8,
              "BLE_MAX_BONDS must be 1..8");
static_assert(BLE_PAIRING_MAX_FAILURES >= 1,
              "BLE_PAIRING_MAX_FAILURES must be non-zero");

// MQTT/STA integration. Set these in ignored LocalConfig.h for a deployment.
#ifndef FIELDRADIO_STA_SSID
#define FIELDRADIO_STA_SSID ""
#endif
#ifndef FIELDRADIO_STA_PASSWORD
#define FIELDRADIO_STA_PASSWORD ""
#endif

#ifndef FIELDRADIO_WIFI_AP_FALLBACK_DELAY_MS
#define FIELDRADIO_WIFI_AP_FALLBACK_DELAY_MS 600000UL
#endif
#ifndef FIELDRADIO_DEVICE_ID
#define FIELDRADIO_DEVICE_ID "ESP32S3_VOICE_NODE_01"
#endif
#ifndef FIELDRADIO_CALLSIGN
#define FIELDRADIO_CALLSIGN "FIELD_RADIO_01"
#endif
constexpr char STA_SSID[] = FIELDRADIO_STA_SSID;
constexpr char STA_PASSWORD[] = FIELDRADIO_STA_PASSWORD;
constexpr size_t STA_SSID_MAX_LEN = 32;
constexpr size_t STA_PASSWORD_MIN_LEN = 8;
constexpr size_t STA_PASSWORD_MAX_LEN = 63;
constexpr char DEVICE_ID[] = FIELDRADIO_DEVICE_ID;
constexpr char DEVICE_CALLSIGN[] = FIELDRADIO_CALLSIGN;
constexpr uint32_t STA_RETRY_MIN_MS = 5000UL;
constexpr uint32_t STA_RETRY_MAX_MS = 300000UL;

#ifndef FIELDRADIO_MQTT_HOST
#define FIELDRADIO_MQTT_HOST "broker.emqx.io"
#endif
#ifndef FIELDRADIO_MQTT_PORT
#define FIELDRADIO_MQTT_PORT 8883
#endif
#ifndef FIELDRADIO_MQTT_USERNAME
#define FIELDRADIO_MQTT_USERNAME ""
#endif
#ifndef FIELDRADIO_MQTT_PASSWORD
#define FIELDRADIO_MQTT_PASSWORD ""
#endif
#ifndef FIELDRADIO_MQTT_TOPIC_ROOT
#define FIELDRADIO_MQTT_TOPIC_ROOT "fieldradio"
#endif
#ifndef FIELDRADIO_MQTT_SERVER_NAME
#define FIELDRADIO_MQTT_SERVER_NAME "broker.emqx.io"
#endif
#if defined(FIELDRADIO_PRODUCTION_BUILD)
#undef FIELDRADIO_MQTT_HOST
#define FIELDRADIO_MQTT_HOST ""
#undef FIELDRADIO_MQTT_USERNAME
#define FIELDRADIO_MQTT_USERNAME ""
#undef FIELDRADIO_MQTT_PASSWORD
#define FIELDRADIO_MQTT_PASSWORD ""
#endif
constexpr char MQTT_HOST[] = FIELDRADIO_MQTT_HOST;
constexpr uint16_t MQTT_PORT = FIELDRADIO_MQTT_PORT;
constexpr char MQTT_USERNAME[] = FIELDRADIO_MQTT_USERNAME;
constexpr char MQTT_PASSWORD[] = FIELDRADIO_MQTT_PASSWORD;
constexpr char MQTT_TOPIC_ROOT[] = FIELDRADIO_MQTT_TOPIC_ROOT;
constexpr char MQTT_SERVER_NAME[] = FIELDRADIO_MQTT_SERVER_NAME;
constexpr uint32_t MQTT_TELEMETRY_PERIOD_MS = 30000UL;
constexpr uint32_t MQTT_HEALTH_PERIOD_MS = 60000UL;
constexpr bool MQTT_LWT_ENABLED = true;
constexpr bool MQTT_RETAIN_AVAILABILITY = true;
constexpr bool MQTT_RETAIN_TELEMETRY = false;
constexpr uint32_t MQTT_RECONNECT_MIN_MS = 5000UL;
constexpr uint32_t MQTT_RECONNECT_MAX_MS = 300000UL;
constexpr uint32_t MQTT_PUBLISH_PERIOD_MS = MQTT_TELEMETRY_PERIOD_MS;

// EST / PKI certificate lifecycle. Disabled by default; deployment supplies the
// vendor-neutral RFC 7030 endpoint. The existing MQTT root CA is also used to
// validate the EST TLS server and issued certificate chain unless the build
// provisioning layer supplies a different generated CA.
#ifndef FIELDRADIO_EST_SERVER_URL
#define FIELDRADIO_EST_SERVER_URL ""
#endif
#ifndef FIELDRADIO_EST_LABEL
#define FIELDRADIO_EST_LABEL "/.well-known/est"
#endif
#ifndef FIELDRADIO_CERT_RENEWAL_THRESHOLD_DAYS
#define FIELDRADIO_CERT_RENEWAL_THRESHOLD_DAYS 30
#endif
#ifndef FIELDRADIO_CERT_CHECK_PERIOD_MS
#define FIELDRADIO_CERT_CHECK_PERIOD_MS 86400000UL
#endif
#ifndef FIELDRADIO_EST_AUTH_MODE
#define FIELDRADIO_EST_AUTH_MODE 0
#endif
#ifndef FIELDRADIO_EST_USERNAME
#define FIELDRADIO_EST_USERNAME ""
#endif
#ifndef FIELDRADIO_EST_PASSWORD
#define FIELDRADIO_EST_PASSWORD ""
#endif
#ifndef FIELDRADIO_EST_BOOTSTRAP_TOKEN
#define FIELDRADIO_EST_BOOTSTRAP_TOKEN ""
#endif
#ifndef FIELDRADIO_CERT_LIFECYCLE_ENABLED
#define FIELDRADIO_CERT_LIFECYCLE_ENABLED 0
#endif
constexpr char EST_SERVER_URL[] = FIELDRADIO_EST_SERVER_URL;
constexpr char EST_LABEL[] = FIELDRADIO_EST_LABEL;
constexpr uint16_t CERT_RENEWAL_THRESHOLD_DAYS = FIELDRADIO_CERT_RENEWAL_THRESHOLD_DAYS;
constexpr uint32_t CERT_CHECK_PERIOD_MS = FIELDRADIO_CERT_CHECK_PERIOD_MS;
constexpr uint8_t EST_AUTH_MODE = FIELDRADIO_EST_AUTH_MODE;
constexpr char EST_USERNAME[] = FIELDRADIO_EST_USERNAME;
constexpr char EST_PASSWORD[] = FIELDRADIO_EST_PASSWORD;
constexpr char EST_BOOTSTRAP_TOKEN[] = FIELDRADIO_EST_BOOTSTRAP_TOKEN;
constexpr bool CERT_LIFECYCLE_ENABLED = FIELDRADIO_CERT_LIFECYCLE_ENABLED;


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
constexpr uint32_t WIFI_AP_IDLE_TIMEOUT_MS = 600000UL;
constexpr uint32_t WIFI_AP_RETRY_MS = 30000UL;
constexpr uint32_t WIFI_AP_FALLBACK_DELAY_MS = FIELDRADIO_WIFI_AP_FALLBACK_DELAY_MS;
constexpr float VOX_THRESHOLD = 0.08f;
constexpr uint32_t VOX_HANG_MS = 700;
constexpr uint32_t VOICE_FRAME_MS = 40;
constexpr uint8_t VOICE_CODEC_VERSION = 2;
constexpr uint8_t VOICE_CODEC2_MODE = 2; // Codec2 1600 bit/s, 40 ms / 320 samples.
constexpr size_t VOICE_CODEC2_BYTES = 8; // 1600 bit/s * 40 ms.
constexpr size_t VOICE_PACKET_BYTES = 4 + 2 + VOICE_CODEC2_BYTES + 2; // hdr+seq+codec+CRC.

constexpr uint32_t AUDIO_TONE_MAX_MS = 2000;
constexpr uint32_t AUDIO_VU_HOLD_MS = 250;

// USB Audio Class device
constexpr char USB_AUDIO_NAME[] = "FieldRadio USB Audio";
}
