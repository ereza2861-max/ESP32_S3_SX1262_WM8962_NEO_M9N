#pragma once
#include <Arduino.h>
#include "Config.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct RuntimeConfig {
  float loraFreqMHz;
  float loraBwKHz;
  uint8_t loraSf;
  uint8_t loraCr;
  uint8_t loraSyncWord;
  int8_t loraPowerDbm;
  uint8_t volume;
  uint8_t audioRecordSource;
  float batteryCalibration;
  String callsign;
  String loraKeyHex;
  String apSsid;
  String apPassword;
  String webUser;
  String webPassword;                 // runtime-only plaintext; never persisted
  String webPasswordSaltHex;           // persisted credential salt
  String webPasswordHashHex;           // persisted credential hash
  uint8_t audioRecordQuality = 2;
  bool lorawanEnabled = false;
  uint8_t lorawanMode = 0;
  uint8_t lorawanRegion = Config::LORAWAN_REGION_DEFAULT;
  String lorawanDevEui;
  String lorawanJoinEui;
  String lorawanAppKey;
  String lorawanNwkSKey;
  String lorawanAppSKey;
  uint8_t lorawanDevAddr[4] = {};
  uint8_t lorawanFPort = Config::LORAWAN_DEFAULT_FPORT;
  uint16_t lorawanUplinkPeriodSec = Config::LORAWAN_UPLINK_PERIOD_SEC_DEFAULT;
  bool blePairingEnabled = Config::BLE_PAIRING_ENABLED_VALUE;
  bool mqttEnabled = true;
  uint32_t wakePeriodSec = Config::GNSS_TIME_SYNC_PERIOD_MS / 1000UL;
  bool classDEnabled = false;
  uint8_t classDBoostLevel = Config::CLASS_D_BOOST_LEVEL;
  // Runtime policy. Hardware-fixed WM8962 output topology remains read-only.
  bool deepSleepEnabled = Config::DEEP_SLEEP_ENABLED;
  uint32_t deepSleepIdleMs = Config::DEEP_SLEEP_IDLE_MS;
  uint32_t deepSleepWakeGraceMs = Config::DEEP_SLEEP_WAKE_GRACE_MS;
  uint32_t criticalShutdownDelayMs = Config::CRITICAL_SHUTDOWN_DELAY_MS;
  float batteryLowThreshold = Config::BATTERY_LOW_THRESHOLD;
  float batteryCriticalThreshold = Config::BATTERY_CRITICAL;

  String mqttHost = Config::MQTT_HOST;
  uint16_t mqttPort = Config::MQTT_PORT;
  bool mqttTlsRequired = true;
  uint32_t mqttReconnectMinMs = Config::MQTT_RECONNECT_MIN_MS;
  uint32_t mqttReconnectMaxMs = Config::MQTT_RECONNECT_MAX_MS;
  uint32_t mqttTelemetryPeriodMs = Config::MQTT_TELEMETRY_PERIOD_MS;
  uint32_t mqttHealthPeriodMs = Config::MQTT_HEALTH_PERIOD_MS;
  bool mqttRetainTelemetry = Config::MQTT_RETAIN_TELEMETRY;
  bool mqttRetainAvailability = Config::MQTT_RETAIN_AVAILABILITY;
  uint16_t mqttCredentialRotationDays = 90; // deprecated compatibility field; not an auth policy
  String estServerUrl = Config::EST_SERVER_URL;
  String estLabel = Config::EST_LABEL;
  uint16_t certRenewalThresholdDays = Config::CERT_RENEWAL_THRESHOLD_DAYS;
  uint32_t certCheckPeriodMs = Config::CERT_CHECK_PERIOD_MS;
  uint8_t estAuthMode = Config::EST_AUTH_MODE;
  String estUsername = Config::EST_USERNAME;
  String estPassword = Config::EST_PASSWORD;
  String estBootstrapToken = Config::EST_BOOTSTRAP_TOKEN;
  bool estBootstrapTokenConsumed = false;
  bool certLifecycleEnabled = Config::CERT_LIFECYCLE_ENABLED;

  bool voxEnabled = false;
  float voxThreshold = Config::VOX_THRESHOLD;
  uint32_t voxHangMs = Config::VOX_HANG_MS;
  bool aecEnabled = Config::AEC_ENABLED_BY_DEFAULT;
  bool usbMonitor = false;
  bool usbPlaybackTransport = false;
  bool audioLoopback = false;

  bool loraAdrEnabled = false;
  bool loraHopEnabled = false;
  uint8_t loraHopChannelProfile = Config::HOP_CHANNEL_MAX;
  bool loraRangeTestMode = false;

  bool sensorReaderEnabled = Config::SENSOR_READER_ENABLED_VALUE;
  uint32_t sensorScanIntervalMs = Config::SENSOR_SCAN_INTERVAL_MS_VALUE;
  uint16_t sensorScanWindowMs = Config::SENSOR_SCAN_WINDOW_MS_VALUE;
  uint32_t sensorScanDurationMs = Config::SENSOR_SCAN_DURATION_MS_VALUE;
  uint32_t sensorConnectTimeoutMs = Config::SENSOR_CONNECT_TIMEOUT_MS_VALUE;
  uint32_t sensorNodeEvictionMs = Config::SENSOR_NODE_EVICTION_MS_VALUE;
  uint8_t sensorMaxNodes = static_cast<uint8_t>(Config::SENSOR_MAX_NODES_VALUE);
  bool sensorRequireEncryption = Config::SENSOR_REQUIRE_ENCRYPTION_VALUE;
  uint8_t blePairingFailureThreshold = Config::BLE_PAIRING_MAX_FAILURES_VALUE;
  uint32_t blePairingBlockMs = Config::BLE_PAIRING_BLOCK_MS_VALUE;
  bool sensorKeepAwake = Config::SENSOR_KEEP_AWAKE_VALUE;

  uint32_t webSessionTimeoutMs = Config::WEB_SESSION_TIMEOUT_MS;
  uint32_t webAuthRateLimitMs = Config::WEB_RATE_LIMIT_MS;
  uint8_t csrfPolicy = 0; // 0=token+Origin, 1=token-only, 2=disabled
  uint8_t blePairingPolicy = 0;
  uint8_t ecdhRekeyPolicy = 0;
  uint8_t replayWindowBits = Config::LORA_REPLAY_WINDOW_BITS;

  void load();
  bool migrate();
  bool save();
  bool setRadio(float freqMHz, float bwKHz, uint8_t sf, uint8_t cr,
                uint8_t syncWord, int8_t powerDbm);
  bool validRadio() const;
  bool validLoRaWAN() const;
  bool webPasswordConfigured() const;
  bool verifyWebPassword(const String& password) const;
};

extern RuntimeConfig gConfig;
extern SemaphoreHandle_t gConfigMutex;
extern std::atomic<uint32_t> gConfigGeneration;

bool configSnapshot(RuntimeConfig& out);
bool configSnapshot(RuntimeConfig& out, uint32_t& generation);
bool configCommit(const RuntimeConfig& candidate);
bool configCommit(const RuntimeConfig& candidate, uint32_t expectedGeneration);
uint32_t configGeneration();
bool configManagerBegin();
