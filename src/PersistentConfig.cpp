#include "PersistentConfig.h"
#include "Config.h"
#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

RuntimeConfig gConfig{
    Config::LORA_FREQ_MHZ,
    Config::LORA_BW_KHZ,
    Config::LORA_SF,
    Config::LORA_CR,
    Config::LORA_SYNC_WORD,
    Config::LORA_POWER_DBM,
    70,
    Config::AUDIO_SOURCE_WM8962_MIC,
    1.0f,
    Config::DEVICE_CALLSIGN,
    Config::LORA_KEY_HEX,
    Config::AP_SSID,
    Config::AP_PASSWORD,
    Config::WEB_USER,
    Config::WEB_PASSWORD,
    "",
    "",
    2};

SemaphoreHandle_t gConfigMutex = nullptr;
std::atomic<uint32_t> gConfigGeneration{0};

bool configSnapshot(RuntimeConfig& out) {
  uint32_t unusedGeneration = 0;
  return configSnapshot(out, unusedGeneration);
}

bool configSnapshot(RuntimeConfig& out, uint32_t& generation) {
  if (!gConfigMutex ||
      xSemaphoreTake(gConfigMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;
  out = gConfig;
  generation = gConfigGeneration.load(std::memory_order_acquire);
  xSemaphoreGive(gConfigMutex);
  return true;
}

bool configCommit(const RuntimeConfig& candidate) {
  return configCommit(candidate, configGeneration());
}

bool configCommit(const RuntimeConfig& candidate, uint32_t expectedGeneration) {
  if (!gConfigMutex ||
      xSemaphoreTake(gConfigMutex, pdMS_TO_TICKS(500)) != pdTRUE)
    return false;
  if (gConfigGeneration.load(std::memory_order_acquire) != expectedGeneration) {
    xSemaphoreGive(gConfigMutex);
    return false;
  }
  const bool ok = candidate.save();
  if (ok) {
    gConfig = candidate;
    uint32_t next = expectedGeneration + 1U;
    if (next == 0) next = 1;
    gConfigGeneration.store(next, std::memory_order_release);
  }
  xSemaphoreGive(gConfigMutex);
  return ok;
}

uint32_t configGeneration() {
  return gConfigGeneration.load(std::memory_order_acquire);
}

namespace {
constexpr char NVS_NS[] = "fieldradio";
constexpr uint32_t CONFIG_VERSION = Config::CONFIG_VERSION;
constexpr size_t PASSWORD_SALT_BYTES = 16;
constexpr uint32_t PASSWORD_HASH_ROUNDS = 10000;
constexpr float MIN_FREQ_MHZ = 920.0f;
constexpr float MAX_FREQ_MHZ = 923.0f;
constexpr float MIN_BW_KHZ = 7.8f;
constexpr float MAX_BW_KHZ = 250.0f;

String hexEncode(const uint8_t* data, size_t len) {
  const char* d = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += d[data[i] >> 4];
    out += d[data[i] & 0x0F];
  }
  return out;
}

bool hexDecode(const String& in, uint8_t* out, size_t len) {
  if (!out || in.length() != len * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = nibble(in[i * 2]);
    const int lo = nibble(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool passwordHash(const String& password, const uint8_t* salt,
                  uint8_t out[32]) {
  if (!salt || !out || password.isEmpty() || password.length() > 63) return false;
  uint8_t state[32] = {};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0 &&
            mbedtls_sha256_update(&ctx, salt, PASSWORD_SALT_BYTES) == 0 &&
            mbedtls_sha256_update(&ctx,
                                  reinterpret_cast<const uint8_t*>(password.c_str()),
                                  password.length()) == 0 &&
            mbedtls_sha256_finish(&ctx, state) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;

  for (uint32_t i = 1; i < PASSWORD_HASH_ROUNDS; ++i) {
    mbedtls_sha256_init(&ctx);
    ok = mbedtls_sha256_starts(&ctx, 0) == 0 &&
         mbedtls_sha256_update(&ctx, state, sizeof(state)) == 0 &&
         mbedtls_sha256_update(&ctx, salt, PASSWORD_SALT_BYTES) == 0 &&
         mbedtls_sha256_finish(&ctx, state) == 0;
    mbedtls_sha256_free(&ctx);
    if (!ok) return false;
  }
  memcpy(out, state, sizeof(state));
  return true;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

bool validCredential(const String& value, size_t maxLen) {
  return !value.isEmpty() && value.length() <= maxLen;
}

bool validHexKey(const String& value) {
  if (value.length() != 32) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool validHexString(const String& value, size_t length) {
  if (value.length() != length) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool validCallsign(const String& value) {
  if (value.isEmpty() || value.length() > 16) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}
}

bool RuntimeConfig::validRadio() const {
  return isfinite(loraFreqMHz) && loraFreqMHz >= MIN_FREQ_MHZ &&
         loraFreqMHz <= MAX_FREQ_MHZ &&
         isfinite(loraBwKHz) && loraBwKHz >= MIN_BW_KHZ &&
         loraBwKHz <= MAX_BW_KHZ &&
         loraSf >= 5 && loraSf <= 12 &&
         loraCr >= 5 && loraCr <= 8 &&
         loraPowerDbm >= 2 && loraPowerDbm <= 17;
}

bool RuntimeConfig::validLoRaWAN() const {
  if (lorawanMode > 1 || lorawanRegion > 3 ||
      lorawanFPort == 0 || lorawanFPort > 223 ||
      lorawanUplinkPeriodSec == 0)
    return false;
  if (!lorawanEnabled) return true;
  if (!validHexString(lorawanDevEui, 16)) return false;
  if (lorawanMode == 0) {
    return validHexString(lorawanJoinEui, 16) &&
           validHexString(lorawanAppKey, 32);
  }
  bool nonZeroAddr = false;
  for (uint8_t b : lorawanDevAddr) nonZeroAddr |= b != 0;
  return nonZeroAddr &&
         validHexString(lorawanNwkSKey, 32) &&
         validHexString(lorawanAppSKey, 32);
}

void RuntimeConfig::load() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return;

  const uint32_t version = prefs.getUInt("cfgver", 0);
  const float freq = prefs.getFloat("freq", loraFreqMHz);
  const float bw = prefs.getFloat("bw", loraBwKHz);
  const uint8_t sf = prefs.getUChar("sf", loraSf);
  const uint8_t cr = prefs.getUChar("cr", loraCr);
  const uint8_t sw = prefs.getUChar("sync", loraSyncWord);
  const int8_t power = prefs.getChar("power", loraPowerDbm);
  const uint8_t savedVolume = prefs.getUChar("volume", volume);
  const uint8_t savedAudioSource = prefs.getUChar("audsrc", audioRecordSource);
  const float battery = prefs.getFloat("batcal", batteryCalibration);
  const String callsignValue = prefs.getString("callsign", callsign);
  const String loraKeyValue = prefs.getString("lorakey", loraKeyHex);
  const String apSsidValue = prefs.getString("apssid", apSsid);
  const String apPasswordValue = prefs.getString("appass", apPassword);
  const String webUserValue = prefs.getString("webuser", webUser);
  const String webPasswordValue = prefs.getString("webpass", "");
  const String webPasswordSaltValue = prefs.getString("websalt", "");
  const String webPasswordHashValue = prefs.getString("webph", "");
  const uint8_t savedRecordQuality = prefs.getUChar("recqual", audioRecordQuality);
  const bool savedLwEnabled = prefs.getBool("lw_enabled", lorawanEnabled);
  const uint8_t savedLwMode = prefs.getUChar("lw_mode", lorawanMode);
  const uint8_t savedLwRegion = prefs.getUChar("lw_region", lorawanRegion);
  const String savedLwDevEui = prefs.getString("lw_deveui", lorawanDevEui);
  const String savedLwJoinEui = prefs.getString("lw_joineui", lorawanJoinEui);
  const String savedLwAppKey = prefs.getString("lw_appkey", lorawanAppKey);
  const String savedLwNwkSKey = prefs.getString("lw_nwkskey", lorawanNwkSKey);
  const String savedLwAppSKey = prefs.getString("lw_appskey", lorawanAppSKey);
  uint8_t savedLwDevAddr[sizeof(lorawanDevAddr)] = {};
  if (prefs.getBytes("lw_devaddr", savedLwDevAddr, sizeof(savedLwDevAddr)) != sizeof(savedLwDevAddr))
    memcpy(savedLwDevAddr, lorawanDevAddr, sizeof(savedLwDevAddr));
  const uint8_t savedLwFPort = prefs.getUChar("lw_fport", lorawanFPort);
  const uint16_t savedLwPeriod = prefs.getUShort("lw_period", lorawanUplinkPeriodSec);
  const bool savedBlePairing = prefs.getBool("ble_pair", blePairingEnabled);
  const bool savedMqttEnabled = prefs.getBool("mqtt_en", mqttEnabled);
  const uint32_t savedWakePeriodSec = prefs.getUInt("wake_sec", wakePeriodSec);
  const bool savedClassDEnabled = prefs.getBool("classd_en", classDEnabled);
  const uint8_t savedClassDBoostLevel = prefs.getUChar("classd_boost", classDBoostLevel);
  const bool savedDeepSleepEnabled = prefs.getBool("sleep_en", deepSleepEnabled);
  const uint32_t savedDeepSleepIdleMs = prefs.getUInt("sleep_idle", deepSleepIdleMs);
  const uint32_t savedWakeGraceMs = prefs.getUInt("wake_grace", deepSleepWakeGraceMs);
  const uint32_t savedCriticalShutdownMs =
      prefs.getUInt("bat_crit_delay", criticalShutdownDelayMs);
  const float savedBatteryLow = prefs.getFloat("bat_low", batteryLowThreshold);
  const float savedBatteryCritical = prefs.getFloat("bat_critical", batteryCriticalThreshold);
  const String savedMqttHost = prefs.getString("mqtt_host", mqttHost);
  const uint16_t savedMqttPort = prefs.getUShort("mqtt_port", mqttPort);
  const bool savedMqttTls = prefs.getBool("mqtt_tls", mqttTlsRequired);
  const uint32_t savedMqttRetryMin = prefs.getUInt("mqtt_rmin", mqttReconnectMinMs);
  const uint32_t savedMqttRetryMax = prefs.getUInt("mqtt_rmax", mqttReconnectMaxMs);
  const uint32_t savedMqttTelemetry = prefs.getUInt("mqtt_tlm", mqttTelemetryPeriodMs);
  const uint32_t savedMqttHealth = prefs.getUInt("mqtt_hlt", mqttHealthPeriodMs);
  const bool savedMqttRetainTelemetry = prefs.getBool("mqtt_rt", mqttRetainTelemetry);
  const bool savedMqttRetainAvailability = prefs.getBool("mqtt_ra", mqttRetainAvailability);
  const uint16_t savedMqttRotation = prefs.getUShort("mqtt_rot", mqttCredentialRotationDays);
  const bool savedVox = prefs.getBool("vox_en", voxEnabled);
  const float savedVoxThreshold = prefs.getFloat("vox_thr", voxThreshold);
  const uint32_t savedVoxHang = prefs.getUInt("vox_hang", voxHangMs);
  const bool savedAec = prefs.getBool("aec_en", aecEnabled);
  const bool savedUsbMonitor = prefs.getBool("usb_mon", usbMonitor);
  const bool savedUsbTransport = prefs.getBool("usb_tx", usbPlaybackTransport);
  const bool savedLoopback = prefs.getBool("loopback", audioLoopback);
  const bool savedAdr = prefs.getBool("lora_adr", loraAdrEnabled);
  const bool savedHop = prefs.getBool("lora_hop", loraHopEnabled);
  const uint8_t savedHopProfile = prefs.getUChar("lora_hprof", loraHopChannelProfile);
  const bool savedRange = prefs.getBool("lora_range", loraRangeTestMode);
  const bool savedSensorEnabled = prefs.getBool("ble_en", sensorReaderEnabled);
  const uint32_t savedScanInterval = prefs.getUInt("ble_si", sensorScanIntervalMs);
  const uint16_t savedScanWindow = prefs.getUShort("ble_sw", sensorScanWindowMs);
  const uint32_t savedScanDuration = prefs.getUInt("ble_sd", sensorScanDurationMs);
  const uint32_t savedConnectTimeout = prefs.getUInt("ble_ct", sensorConnectTimeoutMs);
  const uint32_t savedEviction = prefs.getUInt("ble_ev", sensorNodeEvictionMs);
  const uint8_t savedMaxNodes = prefs.getUChar("ble_max", sensorMaxNodes);
  const bool savedBleEncryption = prefs.getBool("ble_enc", sensorRequireEncryption);
  const uint8_t savedBleFailures = prefs.getUChar("ble_fail", blePairingFailureThreshold);
  const uint32_t savedBleBlock = prefs.getUInt("ble_block", blePairingBlockMs);
  const bool savedKeepAwake = prefs.getBool("ble_awake", sensorKeepAwake);
  const uint32_t savedSessionTimeout = prefs.getUInt("web_sto", webSessionTimeoutMs);
  const uint32_t savedAuthRate = prefs.getUInt("web_rl", webAuthRateLimitMs);
  const uint8_t savedCsrf = prefs.getUChar("web_csrf", csrfPolicy);
  const uint8_t savedPairPolicy = prefs.getUChar("ble_policy", blePairingPolicy);
  const uint8_t savedEcdhPolicy = prefs.getUChar("ecdh_policy", ecdhRekeyPolicy);
  const uint8_t savedReplayWindow = prefs.getUChar("replay_win", replayWindowBits);
  prefs.end();

  RuntimeConfig candidate = *this;
  candidate.loraFreqMHz = freq;
  candidate.loraBwKHz = bw;
  candidate.loraSf = sf;
  candidate.loraCr = cr;
  candidate.loraSyncWord = sw;
  candidate.loraPowerDbm = power;
  candidate.volume = savedVolume;
  candidate.audioRecordSource = savedAudioSource;
  candidate.batteryCalibration = battery;
  candidate.callsign = callsignValue;
  candidate.loraKeyHex = loraKeyValue;
  candidate.apSsid = apSsidValue;
  candidate.apPassword = apPasswordValue;
  candidate.webUser = webUserValue;
  candidate.webPassword = webPasswordValue;
  candidate.webPasswordSaltHex = webPasswordSaltValue;
  candidate.webPasswordHashHex = webPasswordHashValue;
  candidate.audioRecordQuality = savedRecordQuality;
  candidate.lorawanEnabled = savedLwEnabled;
  candidate.lorawanMode = savedLwMode;
  candidate.lorawanRegion = savedLwRegion;
  candidate.lorawanDevEui = savedLwDevEui;
  candidate.lorawanJoinEui = savedLwJoinEui;
  candidate.lorawanAppKey = savedLwAppKey;
  candidate.lorawanNwkSKey = savedLwNwkSKey;
  candidate.lorawanAppSKey = savedLwAppSKey;
  memcpy(candidate.lorawanDevAddr, savedLwDevAddr, sizeof(candidate.lorawanDevAddr));
  candidate.lorawanFPort = savedLwFPort;
  candidate.lorawanUplinkPeriodSec = savedLwPeriod;
  candidate.blePairingEnabled = savedBlePairing;
  candidate.mqttEnabled = savedMqttEnabled;
  candidate.wakePeriodSec = savedWakePeriodSec;
  candidate.classDEnabled = savedClassDEnabled;
  candidate.classDBoostLevel = savedClassDBoostLevel;
  candidate.deepSleepEnabled = savedDeepSleepEnabled;
  candidate.deepSleepIdleMs = savedDeepSleepIdleMs;
  candidate.deepSleepWakeGraceMs = savedWakeGraceMs;
  candidate.criticalShutdownDelayMs = savedCriticalShutdownMs;
  candidate.batteryLowThreshold = savedBatteryLow;
  candidate.batteryCriticalThreshold = savedBatteryCritical;
  candidate.mqttHost = savedMqttHost;
  candidate.mqttPort = savedMqttPort;
  candidate.mqttTlsRequired = savedMqttTls;
  candidate.mqttReconnectMinMs = savedMqttRetryMin;
  candidate.mqttReconnectMaxMs = savedMqttRetryMax;
  candidate.mqttTelemetryPeriodMs = savedMqttTelemetry;
  candidate.mqttHealthPeriodMs = savedMqttHealth;
  candidate.mqttRetainTelemetry = savedMqttRetainTelemetry;
  candidate.mqttRetainAvailability = savedMqttRetainAvailability;
  candidate.mqttCredentialRotationDays = savedMqttRotation;
  candidate.voxEnabled = savedVox;
  candidate.voxThreshold = savedVoxThreshold;
  candidate.voxHangMs = savedVoxHang;
  candidate.aecEnabled = savedAec;
  candidate.usbMonitor = savedUsbMonitor;
  candidate.usbPlaybackTransport = savedUsbTransport;
  candidate.audioLoopback = savedLoopback;
  candidate.loraAdrEnabled = savedAdr;
  candidate.loraHopEnabled = savedHop;
  candidate.loraHopChannelProfile = savedHopProfile;
  candidate.loraRangeTestMode = savedRange;
  candidate.sensorReaderEnabled = savedSensorEnabled;
  candidate.sensorScanIntervalMs = savedScanInterval;
  candidate.sensorScanWindowMs = savedScanWindow;
  candidate.sensorScanDurationMs = savedScanDuration;
  candidate.sensorConnectTimeoutMs = savedConnectTimeout;
  candidate.sensorNodeEvictionMs = savedEviction;
  candidate.sensorMaxNodes = savedMaxNodes;
  candidate.sensorRequireEncryption = savedBleEncryption;
  candidate.blePairingFailureThreshold = savedBleFailures;
  candidate.blePairingBlockMs = savedBleBlock;
  candidate.sensorKeepAwake = savedKeepAwake;
  candidate.webSessionTimeoutMs = savedSessionTimeout;
  candidate.webAuthRateLimitMs = savedAuthRate;
  candidate.csrfPolicy = savedCsrf;
  candidate.blePairingPolicy = savedPairPolicy;
  candidate.ecdhRekeyPolicy = savedEcdhPolicy;
  candidate.replayWindowBits = savedReplayWindow;
  if (version == 4) {
    // v4 had no BLE pairing field. Keep legacy values and initialize the
    // pairing flag from the compile-time default; no passkey is migrated.
    candidate.blePairingEnabled = Config::BLE_PAIRING_ENABLED_VALUE;
  }

  if (candidate.validRadio()) {
    loraFreqMHz = candidate.loraFreqMHz;
    loraBwKHz = candidate.loraBwKHz;
    loraSf = candidate.loraSf;
    loraCr = candidate.loraCr;
    loraSyncWord = candidate.loraSyncWord;
    loraPowerDbm = candidate.loraPowerDbm;
  }
  if (candidate.volume <= 100) volume = candidate.volume;
  if (candidate.audioRecordQuality <= 2) audioRecordQuality = candidate.audioRecordQuality;
  if (candidate.audioRecordSource <= Config::AUDIO_SOURCE_USB) {
    audioRecordSource = candidate.audioRecordSource;
  }
  if (isfinite(candidate.batteryCalibration) &&
      candidate.batteryCalibration >= 0.5f &&
      candidate.batteryCalibration <= 1.5f)
    batteryCalibration = candidate.batteryCalibration;
  if (validCallsign(candidate.callsign)) callsign = candidate.callsign;
  if (validHexKey(candidate.loraKeyHex)) loraKeyHex = candidate.loraKeyHex;
  if (validCredential(candidate.apSsid, 32)) apSsid = candidate.apSsid;
  if (candidate.apPassword.length() <= 63) apPassword = candidate.apPassword;
  if (validCredential(candidate.webUser, 32)) webUser = candidate.webUser;
  if (candidate.validLoRaWAN()) {
    lorawanEnabled = candidate.lorawanEnabled;
    lorawanMode = candidate.lorawanMode;
    lorawanRegion = candidate.lorawanRegion;
    lorawanDevEui = candidate.lorawanDevEui;
    lorawanJoinEui = candidate.lorawanJoinEui;
    lorawanAppKey = candidate.lorawanAppKey;
    lorawanNwkSKey = candidate.lorawanNwkSKey;
    lorawanAppSKey = candidate.lorawanAppSKey;
    memcpy(lorawanDevAddr, candidate.lorawanDevAddr, sizeof(lorawanDevAddr));
    lorawanFPort = candidate.lorawanFPort;
    lorawanUplinkPeriodSec = candidate.lorawanUplinkPeriodSec;
  }
  blePairingEnabled = candidate.blePairingEnabled;
  mqttEnabled = candidate.mqttEnabled;
  if (candidate.wakePeriodSec >= 60UL && candidate.wakePeriodSec <= 7UL * 24UL * 60UL * 60UL)
    wakePeriodSec = candidate.wakePeriodSec;
  if (candidate.classDBoostLevel <= 7)
    classDBoostLevel = candidate.classDBoostLevel;
  classDEnabled = candidate.classDEnabled && Config::CLASS_D_ENABLED;
  if (candidate.deepSleepIdleMs >= 60000UL &&
      candidate.deepSleepIdleMs <= 24UL * 60UL * 60UL * 1000UL)
    deepSleepIdleMs = candidate.deepSleepIdleMs;
  if (candidate.deepSleepWakeGraceMs >= 100UL &&
      candidate.deepSleepWakeGraceMs <= 60000UL)
    deepSleepWakeGraceMs = candidate.deepSleepWakeGraceMs;
  if (candidate.criticalShutdownDelayMs >= 100UL &&
      candidate.criticalShutdownDelayMs <= 600000UL)
    criticalShutdownDelayMs = candidate.criticalShutdownDelayMs;
  if (isfinite(candidate.batteryLowThreshold) &&
      isfinite(candidate.batteryCriticalThreshold) &&
      candidate.batteryCriticalThreshold >= 2.5f &&
      candidate.batteryLowThreshold > candidate.batteryCriticalThreshold &&
      candidate.batteryLowThreshold <= 4.2f)
    {
      batteryLowThreshold = candidate.batteryLowThreshold;
      batteryCriticalThreshold = candidate.batteryCriticalThreshold;
    }
  deepSleepEnabled = candidate.deepSleepEnabled;
  if (candidate.mqttHost.length() <= 253 && candidate.mqttHost.indexOf('|') < 0)
    mqttHost = candidate.mqttHost;
  if (candidate.mqttPort > 0) mqttPort = candidate.mqttPort;
  if (candidate.mqttReconnectMinMs >= 1000UL &&
      candidate.mqttReconnectMaxMs >= candidate.mqttReconnectMinMs &&
      candidate.mqttReconnectMaxMs <= 3600000UL) {
    mqttReconnectMinMs = candidate.mqttReconnectMinMs;
    mqttReconnectMaxMs = candidate.mqttReconnectMaxMs;
  }
  if (candidate.mqttTelemetryPeriodMs >= 1000UL && candidate.mqttTelemetryPeriodMs <= 86400000UL)
    mqttTelemetryPeriodMs = candidate.mqttTelemetryPeriodMs;
  if (candidate.mqttHealthPeriodMs >= 1000UL && candidate.mqttHealthPeriodMs <= 86400000UL)
    mqttHealthPeriodMs = candidate.mqttHealthPeriodMs;
  mqttTlsRequired = candidate.mqttTlsRequired;
  mqttRetainTelemetry = candidate.mqttRetainTelemetry;
  mqttRetainAvailability = candidate.mqttRetainAvailability;
  if (candidate.mqttCredentialRotationDays >= 1 && candidate.mqttCredentialRotationDays <= 3650)
    mqttCredentialRotationDays = candidate.mqttCredentialRotationDays;
  voxEnabled = candidate.voxEnabled;
  if (candidate.voxThreshold >= 0.005f && candidate.voxThreshold <= 1.0f) voxThreshold = candidate.voxThreshold;
  if (candidate.voxHangMs >= 50U && candidate.voxHangMs <= 10000U) voxHangMs = candidate.voxHangMs;
  aecEnabled = candidate.aecEnabled;
  usbMonitor = candidate.usbMonitor;
  usbPlaybackTransport = candidate.usbPlaybackTransport;
  audioLoopback = candidate.audioLoopback;
  loraAdrEnabled = candidate.loraAdrEnabled;
  loraHopEnabled = candidate.loraHopEnabled;
  if (candidate.loraHopChannelProfile >= 1 && candidate.loraHopChannelProfile <= Config::HOP_CHANNEL_MAX)
    loraHopChannelProfile = candidate.loraHopChannelProfile;
  loraRangeTestMode = candidate.loraRangeTestMode;
  sensorReaderEnabled = candidate.sensorReaderEnabled;
  if (candidate.sensorScanIntervalMs >= 100 && candidate.sensorScanIntervalMs <= 60000 &&
      candidate.sensorScanWindowMs > 0 && candidate.sensorScanWindowMs <= candidate.sensorScanIntervalMs) {
    sensorScanIntervalMs = candidate.sensorScanIntervalMs;
    sensorScanWindowMs = candidate.sensorScanWindowMs;
  }
  if (candidate.sensorScanDurationMs >= 100 && candidate.sensorScanDurationMs <= 60000)
    sensorScanDurationMs = candidate.sensorScanDurationMs;
  if (candidate.sensorConnectTimeoutMs >= 500 && candidate.sensorConnectTimeoutMs <= 30000)
    sensorConnectTimeoutMs = candidate.sensorConnectTimeoutMs;
  if (candidate.sensorNodeEvictionMs >= 10000 && candidate.sensorNodeEvictionMs <= 7UL * 86400000UL)
    sensorNodeEvictionMs = candidate.sensorNodeEvictionMs;
  if (candidate.sensorMaxNodes >= 1 && candidate.sensorMaxNodes <= Config::SENSOR_MAX_NODES_VALUE)
    sensorMaxNodes = candidate.sensorMaxNodes;
  sensorRequireEncryption = candidate.sensorRequireEncryption;
  if (candidate.blePairingFailureThreshold >= 1 && candidate.blePairingFailureThreshold <= 20)
    blePairingFailureThreshold = candidate.blePairingFailureThreshold;
  if (candidate.blePairingBlockMs >= 1000 && candidate.blePairingBlockMs <= 86400000UL)
    blePairingBlockMs = candidate.blePairingBlockMs;
  sensorKeepAwake = candidate.sensorKeepAwake;
  if (candidate.webSessionTimeoutMs >= 60000UL && candidate.webSessionTimeoutMs <= 86400000UL)
    webSessionTimeoutMs = candidate.webSessionTimeoutMs;
  if (candidate.webAuthRateLimitMs >= 100 && candidate.webAuthRateLimitMs <= 600000UL)
    webAuthRateLimitMs = candidate.webAuthRateLimitMs;
  // CSRF-disabled mode is not a production-safe runtime policy. Treat
  // legacy/invalid value 2 as the strict default during load.
  if (candidate.csrfPolicy <= 1) csrfPolicy = candidate.csrfPolicy;
  else csrfPolicy = 0;
  if (candidate.blePairingPolicy <= 1) blePairingPolicy = candidate.blePairingPolicy;
  if (candidate.ecdhRekeyPolicy <= 1) ecdhRekeyPolicy = candidate.ecdhRekeyPolicy;
  // Keep the runtime replay window inside the bitmap capacity. A minimum of
  // 8 bits avoids making the anti-replay policy trivially permissive.
  if (candidate.replayWindowBits >= 8 &&
      candidate.replayWindowBits <= Config::LORA_REPLAY_WINDOW_BITS)
    replayWindowBits = candidate.replayWindowBits;
  if (candidate.webPassword.length() <= 63) webPassword = candidate.webPassword;
  uint8_t passwordSaltCheck[PASSWORD_SALT_BYTES] = {};
  if (hexDecode(candidate.webPasswordSaltHex, passwordSaltCheck, sizeof(passwordSaltCheck)) &&
      candidate.webPasswordHashHex.length() == 64) {
    webPasswordSaltHex = candidate.webPasswordSaltHex;
    webPasswordHashHex = candidate.webPasswordHashHex;
    if (candidate.webPassword.isEmpty()) webPassword.clear();
  }

  // One-time migration: remove the legacy plaintext web password from NVS.
  // Runtime memory may temporarily contain the password because HTTP Basic
  // authentication still needs the cleartext value until the next reboot.
  if (!webPassword.isEmpty()) {
    (void)save();
  } else if (version == 4) {
    (void)save();
  } else if (version != CONFIG_VERSION && webPasswordConfigured()) {
    (void)save();
  }
}

bool RuntimeConfig::migrate() {
  load();
  return save();
}

bool RuntimeConfig::save() const {
  if (!validRadio() || volume > 100 || audioRecordSource > Config::AUDIO_SOURCE_USB ||
      audioRecordQuality > 2 || classDBoostLevel > 7 ||
      wakePeriodSec < 60UL || wakePeriodSec > 7UL * 24UL * 60UL * 60UL ||
      (classDEnabled && !Config::CLASS_D_ENABLED) ||
      deepSleepIdleMs < 60000UL ||
      deepSleepIdleMs > 24UL * 60UL * 60UL * 1000UL ||
      deepSleepWakeGraceMs < 100UL ||
      deepSleepWakeGraceMs > 60000UL ||
      criticalShutdownDelayMs < 100UL ||
      criticalShutdownDelayMs > 600000UL ||
      !isfinite(batteryLowThreshold) ||
      !isfinite(batteryCriticalThreshold) ||
      batteryCriticalThreshold < 2.5f ||
      batteryLowThreshold <= batteryCriticalThreshold ||
      batteryLowThreshold > 4.2f ||
      (mqttEnabled && (mqttHost.isEmpty() || mqttHost.length() > 253 || mqttPort == 0)) ||
      mqttReconnectMinMs < 1000UL || mqttReconnectMaxMs < mqttReconnectMinMs ||
      mqttReconnectMaxMs > 3600000UL ||
      mqttTelemetryPeriodMs < 1000UL || mqttTelemetryPeriodMs > 86400000UL ||
      mqttHealthPeriodMs < 1000UL || mqttHealthPeriodMs > 86400000UL ||
      mqttCredentialRotationDays < 1 || mqttCredentialRotationDays > 3650 ||
      voxThreshold < 0.005f || voxThreshold > 1.0f ||
      voxHangMs < 50U || voxHangMs > 10000U ||
      loraHopChannelProfile < 1 || loraHopChannelProfile > Config::HOP_CHANNEL_MAX ||
      sensorScanIntervalMs < 100 || sensorScanIntervalMs > 60000 ||
      sensorScanWindowMs == 0 || sensorScanWindowMs > sensorScanIntervalMs ||
      sensorScanDurationMs < 100 || sensorScanDurationMs > 60000 ||
      sensorConnectTimeoutMs < 500 || sensorConnectTimeoutMs > 30000 ||
      sensorNodeEvictionMs < 10000 || sensorNodeEvictionMs > 7UL * 86400000UL ||
      sensorMaxNodes < 1 || sensorMaxNodes > Config::SENSOR_MAX_NODES_VALUE ||
      blePairingFailureThreshold < 1 || blePairingFailureThreshold > 20 ||
      blePairingBlockMs < 1000 || blePairingBlockMs > 86400000UL ||
      webSessionTimeoutMs < 60000UL || webSessionTimeoutMs > 86400000UL ||
      webAuthRateLimitMs < 100 || webAuthRateLimitMs > 600000UL ||
      csrfPolicy > 1 || blePairingPolicy > 1 || ecdhRekeyPolicy > 1 ||
      replayWindowBits < 8 || replayWindowBits > Config::LORA_REPLAY_WINDOW_BITS ||
      !isfinite(batteryCalibration) ||
      batteryCalibration < 0.5f || batteryCalibration > 1.5f ||
      !validLoRaWAN() ||
!validCallsign(callsign) || !validHexKey(loraKeyHex) || apSsid.isEmpty() || apSsid.length() > 32 ||
      apPassword.length() < 8 || apPassword.length() > 63 ||
      webUser.isEmpty() || webUser.length() > 32 ||
      !webPasswordConfigured())
    return false;

  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  bool ok = prefs.putUInt("cfgver", CONFIG_VERSION) > 0 &&
            prefs.putFloat("freq", loraFreqMHz) &&
            prefs.putFloat("bw", loraBwKHz) &&
            prefs.putUChar("sf", loraSf) &&
            prefs.putUChar("cr", loraCr) &&
            prefs.putUChar("sync", loraSyncWord) &&
            prefs.putChar("power", loraPowerDbm) &&
            prefs.putUChar("volume", volume) &&
            prefs.putUChar("audsrc", audioRecordSource) &&
            prefs.putFloat("batcal", batteryCalibration) &&
            prefs.putString("callsign", callsign) > 0 &&
            prefs.putString("lorakey", loraKeyHex) > 0 &&
            prefs.putString("apssid", apSsid) > 0 &&
            prefs.putString("appass", apPassword) > 0 &&
            prefs.putString("webuser", webUser) > 0 &&
            prefs.putUChar("recqual", audioRecordQuality) > 0 &&
            prefs.putBool("lw_enabled", lorawanEnabled) &&
            prefs.putUChar("lw_mode", lorawanMode) > 0 &&
            prefs.putUChar("lw_region", lorawanRegion) > 0 &&
            prefs.putString("lw_deveui", lorawanDevEui) > 0 &&
            prefs.putString("lw_joineui", lorawanJoinEui) > 0 &&
            prefs.putString("lw_appkey", lorawanAppKey) > 0 &&
            prefs.putString("lw_nwkskey", lorawanNwkSKey) > 0 &&
            prefs.putString("lw_appskey", lorawanAppSKey) > 0 &&
            prefs.putBytes("lw_devaddr", lorawanDevAddr, sizeof(lorawanDevAddr)) == sizeof(lorawanDevAddr) &&
            prefs.putUChar("lw_fport", lorawanFPort) > 0 &&
            prefs.putUShort("lw_period", lorawanUplinkPeriodSec) > 0 &&
            prefs.putBool("ble_pair", blePairingEnabled) &&
             prefs.putBool("mqtt_en", mqttEnabled) &&
             prefs.putUInt("wake_sec", wakePeriodSec) > 0 &&
             prefs.putBool("classd_en", classDEnabled) &&
             prefs.putUChar("classd_boost", classDBoostLevel) > 0 &&
             prefs.putBool("sleep_en", deepSleepEnabled) &&
             prefs.putUInt("sleep_idle", deepSleepIdleMs) > 0 &&
             prefs.putUInt("wake_grace", deepSleepWakeGraceMs) > 0 &&
             prefs.putUInt("bat_crit_delay", criticalShutdownDelayMs) > 0 &&
             prefs.putFloat("bat_low", batteryLowThreshold) != 0 &&
             prefs.putFloat("bat_critical", batteryCriticalThreshold) != 0 &&
             prefs.putString("mqtt_host", mqttHost) > 0 &&
             prefs.putUShort("mqtt_port", mqttPort) > 0 &&
             prefs.putBool("mqtt_tls", mqttTlsRequired) &&
             prefs.putUInt("mqtt_rmin", mqttReconnectMinMs) > 0 &&
             prefs.putUInt("mqtt_rmax", mqttReconnectMaxMs) > 0 &&
             prefs.putUInt("mqtt_tlm", mqttTelemetryPeriodMs) > 0 &&
             prefs.putUInt("mqtt_hlt", mqttHealthPeriodMs) > 0 &&
             prefs.putBool("mqtt_rt", mqttRetainTelemetry) &&
             prefs.putBool("mqtt_ra", mqttRetainAvailability) &&
             prefs.putUShort("mqtt_rot", mqttCredentialRotationDays) > 0 &&
             prefs.putBool("vox_en", voxEnabled) &&
             prefs.putFloat("vox_thr", voxThreshold) != 0 &&
             prefs.putUInt("vox_hang", voxHangMs) > 0 &&
             prefs.putBool("aec_en", aecEnabled) &&
             prefs.putBool("usb_mon", usbMonitor) &&
             prefs.putBool("usb_tx", usbPlaybackTransport) &&
             prefs.putBool("loopback", audioLoopback) &&
             prefs.putBool("lora_adr", loraAdrEnabled) &&
             prefs.putBool("lora_hop", loraHopEnabled) &&
             prefs.putUChar("lora_hprof", loraHopChannelProfile) > 0 &&
             prefs.putBool("lora_range", loraRangeTestMode) &&
             prefs.putBool("ble_en", sensorReaderEnabled) &&
             prefs.putUInt("ble_si", sensorScanIntervalMs) > 0 &&
             prefs.putUShort("ble_sw", sensorScanWindowMs) > 0 &&
             prefs.putUInt("ble_sd", sensorScanDurationMs) > 0 &&
             prefs.putUInt("ble_ct", sensorConnectTimeoutMs) > 0 &&
             prefs.putUInt("ble_ev", sensorNodeEvictionMs) > 0 &&
             prefs.putUChar("ble_max", sensorMaxNodes) > 0 &&
             prefs.putBool("ble_enc", sensorRequireEncryption) &&
             prefs.putUChar("ble_fail", blePairingFailureThreshold) > 0 &&
             prefs.putUInt("ble_block", blePairingBlockMs) > 0 &&
             prefs.putBool("ble_awake", sensorKeepAwake) &&
             prefs.putUInt("web_sto", webSessionTimeoutMs) > 0 &&
             prefs.putUInt("web_rl", webAuthRateLimitMs) > 0 &&
             prefs.putUChar("web_csrf", csrfPolicy) > 0 &&
             prefs.putUChar("ble_policy", blePairingPolicy) > 0 &&
             prefs.putUChar("ecdh_policy", ecdhRekeyPolicy) > 0 &&
             prefs.putUChar("replay_win", replayWindowBits) > 0;
  if (ok) {
    uint8_t salt[PASSWORD_SALT_BYTES] = {};
    uint8_t hash[32] = {};
    bool haveCredential = false;
    if (!webPassword.isEmpty()) {
      for (size_t i = 0; i < sizeof(salt); i += 4) {
        const uint32_t r = esp_random();
        memcpy(salt + i, &r, min<size_t>(4, sizeof(salt) - i));
      }
      haveCredential = passwordHash(webPassword, salt, hash);
      if (haveCredential) {
        ok = prefs.putString("websalt", hexEncode(salt, sizeof(salt))) > 0 &&
             prefs.putString("webph", hexEncode(hash, sizeof(hash))) > 0;
      }
    } else {
      uint8_t existingSalt[PASSWORD_SALT_BYTES] = {};
      haveCredential = hexDecode(webPasswordSaltHex, existingSalt, sizeof(existingSalt)) &&
                       webPasswordHashHex.length() == 64;
      ok = haveCredential &&
           prefs.putString("websalt", webPasswordSaltHex) > 0 &&
           prefs.putString("webph", webPasswordHashHex) > 0;
    }
    // Deliberately remove the legacy plaintext key on every successful save.
    if (ok) (void)prefs.remove("webpass");
  }
  prefs.end();
  return ok;
}

bool RuntimeConfig::webPasswordConfigured() const {
  if (webPassword.length() >= 8 && webPassword.length() <= 63) return true;
  uint8_t salt[PASSWORD_SALT_BYTES] = {};
  uint8_t hash[32] = {};
  return hexDecode(webPasswordSaltHex, salt, sizeof(salt)) &&
         webPasswordHashHex.length() == 64 &&
         hexDecode(webPasswordHashHex, hash, sizeof(hash));
}

bool RuntimeConfig::verifyWebPassword(const String& password) const {
  if (password.length() < 8 || password.length() > 63) return false;
  if (!webPassword.isEmpty()) {
    if (password.length() != webPassword.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < password.length(); ++i)
      diff |= static_cast<uint8_t>(password[i] ^ webPassword[i]);
    return diff == 0;
  }
  uint8_t salt[PASSWORD_SALT_BYTES] = {};
  uint8_t expected[32] = {};
  uint8_t got[32] = {};
  if (!hexDecode(webPasswordSaltHex, salt, sizeof(salt)) ||
      !hexDecode(webPasswordHashHex, expected, sizeof(expected)) ||
      !passwordHash(password, salt, got))
    return false;
  return constantTimeEqual(expected, got, sizeof(expected));
}

bool RuntimeConfig::setRadio(float freqMHz, float bwKHz, uint8_t sf,
                             uint8_t cr, uint8_t syncWord, int8_t powerDbm) {
  RuntimeConfig candidate = *this;
  candidate.loraFreqMHz = freqMHz;
  candidate.loraBwKHz = bwKHz;
  candidate.loraSf = sf;
  candidate.loraCr = cr;
  candidate.loraSyncWord = syncWord;
  candidate.loraPowerDbm = powerDbm;
  if (!candidate.validRadio()) return false;
  *this = candidate;
  return true;
}
