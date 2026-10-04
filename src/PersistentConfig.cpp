#include "PersistentConfig.h"
#include "Config.h"
#include <Preferences.h>
#include "MramStorage.h"
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdint>
#include <functional>

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
SemaphoreHandle_t gConfigTransactionMutex = nullptr;
std::atomic<uint32_t> gConfigGeneration{0};

namespace {
struct ConfigCommand {
  const RuntimeConfig* candidate = nullptr;
  uint32_t expectedGeneration = 0;
  SemaphoreHandle_t done = nullptr;
  bool* result = nullptr;
};
constexpr size_t CONFIG_COMMAND_QUEUE_DEPTH = 4;
StaticQueue_t configQueueStruct{};
uint8_t configQueueStorage[CONFIG_COMMAND_QUEUE_DEPTH * sizeof(ConfigCommand)]{};
QueueHandle_t configQueue = nullptr;
TaskHandle_t configTaskHandle = nullptr;

void configManagerTask(void*) {
  ConfigCommand command{};
  for (;;) {
    if (xQueueReceive(configQueue, &command, portMAX_DELAY) != pdTRUE ||
        !command.candidate || !command.done || !command.result) continue;
    bool ok = false;
    if (gConfigMutex && xSemaphoreTake(gConfigMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      if (gConfigGeneration.load(std::memory_order_acquire) == command.expectedGeneration) {
        RuntimeConfig normalized = *command.candidate;
        ok = normalized.save();
        if (ok) {
          gConfig = normalized;
        }
      }
      xSemaphoreGive(gConfigMutex);
    }
    *command.result = ok;
    (void)xSemaphoreGive(command.done);
  }
}
} // namespace

namespace {
constexpr char NVS_NS[] = "fieldradio";
constexpr char NVS_TXN_STATE[] = "cfg_txn_state";
constexpr char NVS_TXN_PREV_GEN[] = "cfg_txn_prev";
constexpr char NVS_TXN_CANDIDATE_GEN[] = "cfg_txn_candidate";
constexpr uint8_t CONFIG_TXN_PENDING = 0xC1;
constexpr uint8_t CONFIG_TXN_COMMITTED = 0xC2;
}

// ENH-2: Map transaction events to the required serial audit names.
namespace {
#pragma pack(push, 1)
struct ConfigTxnMramRecord {
  uint32_t magic = 0x4354584AUL; // CTXJ
  uint8_t version = 1;
  uint8_t event = 0;
  uint16_t reserved = 0;
  uint32_t generation = 0;
  uint32_t timestampMs = 0;
  char detail[32] = {};
  uint32_t crc = 0;
};
#pragma pack(pop)
static_assert(sizeof(ConfigTxnMramRecord) == Config::CONFIG_TXN_MRAM_RECORD_BYTES,
              "config txn MRAM record size mismatch");

uint32_t configTxnMramCrc(const ConfigTxnMramRecord& record) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&record);
  uint32_t c = 0xFFFFFFFFUL;
  for (size_t i = 0; i < offsetof(ConfigTxnMramRecord, crc); ++i) {
    c ^= p[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      c = (c & 1U) ? (c >> 1U) ^ 0xEDB88320UL : c >> 1U;
  }
  return ~c;
}

bool configTxnMramRecordValid(const ConfigTxnMramRecord& record) {
  return record.magic == 0x4354584AUL && record.version == 1 &&
         record.crc == configTxnMramCrc(record);
}

size_t configTxnMramCount() {
  MramStorage& mram = MramStorage::shared();
  if (!mram.begin()) return 0;
  size_t count = 0;
  for (uint8_t i = 0; i < Config::CONFIG_TXN_MRAM_RECORD_COUNT; ++i) {
    ConfigTxnMramRecord record{};
    if (!mram.read(static_cast<uint16_t>(
                       Config::CONFIG_TXN_MRAM_ADDR +
                       i * Config::CONFIG_TXN_MRAM_RECORD_BYTES),
                   &record, sizeof(record))) break;
    if (!configTxnMramRecordValid(record)) break;
    ++count;
  }
  return count;
}

bool appendConfigTxnMram(ConfigTxnEvent event, uint32_t generation,
                         const char* detail) {
  MramStorage& mram = MramStorage::shared();
  if (!mram.begin()) return false;
  for (uint8_t i = 0; i < Config::CONFIG_TXN_MRAM_RECORD_COUNT; ++i) {
    ConfigTxnMramRecord existing{};
    const uint16_t address = static_cast<uint16_t>(
        Config::CONFIG_TXN_MRAM_ADDR +
        i * Config::CONFIG_TXN_MRAM_RECORD_BYTES);
    if (!mram.read(address, &existing, sizeof(existing))) return false;
    if (configTxnMramRecordValid(existing)) continue;

    ConfigTxnMramRecord record{};
    record.event = static_cast<uint8_t>(event);
    record.generation = generation;
    record.timestampMs = millis();
    if (detail) std::strncpy(record.detail, detail, sizeof(record.detail) - 1);
    record.crc = configTxnMramCrc(record);
    return mram.write(address, &record, sizeof(record));
  }
  return false;
}
}  // namespace

// ENH-2: Map transaction events to the required serial audit names and persist
// the same event append-only in MRAM.
void configTxnAudit(ConfigTxnEvent event, uint32_t generation,
                    const char* detail) {
  const char* name = "Unknown";
  switch (event) {
    case ConfigTxnEvent::Pending: name = "Pending"; break;
    case ConfigTxnEvent::Committed: name = "Committed"; break;
    case ConfigTxnEvent::Applied: name = "Applied"; break;
    case ConfigTxnEvent::ApplyFailed: name = "ApplyFailed"; break;
    case ConfigTxnEvent::RolledBack: name = "RolledBack"; break;
    case ConfigTxnEvent::JournalCleared: name = "JournalCleared"; break;
    case ConfigTxnEvent::Recovered: name = "Recovered"; break;
    case ConfigTxnEvent::EcdhRejectedSecurity: name = "ECDH_REJECTED_SECURITY"; break;
  }
  Serial.printf("[CFG-TXN] event=%s gen=%lu detail=%s\n",
                name, static_cast<unsigned long>(generation),
                detail ? detail : "");
  (void)appendConfigTxnMram(event, generation, detail);
}

String configTxnJournalStatusJson() {
  const size_t count = configTxnMramCount();
  String out = "{\"records\":";
  out += String(static_cast<unsigned>(count));
  out += ",\"capacity\":" + String(static_cast<unsigned>(Config::CONFIG_TXN_MRAM_RECORD_COUNT));
  out += ",\"appendOnly\":true}";
  return out;
}

bool configSnapshot(RuntimeConfig& out) { uint32_t generation = 0; return configSnapshot(out, generation); }

bool configSnapshot(RuntimeConfig& out, uint32_t& generation) {
  if (!gConfigMutex || xSemaphoreTake(gConfigMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  out = gConfig;
  generation = gConfigGeneration.load(std::memory_order_acquire);
  xSemaphoreGive(gConfigMutex);
  return true;
}

namespace {
bool configCommitInternal(const RuntimeConfig& candidate, uint32_t expectedGeneration) {
  if (!configQueue || !gConfigMutex) return false;
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) return false;
  bool result = false;
  ConfigCommand command{&candidate, expectedGeneration, done, &result};
  if (xQueueSend(configQueue, &command, pdMS_TO_TICKS(500)) != pdTRUE) {
    vSemaphoreDelete(done);
    return false;
  }
  (void)xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);
  return result;
}
} // namespace

bool configCommit(const RuntimeConfig& candidate) { return configCommit(candidate, configGeneration()); }

bool configCommit(const RuntimeConfig& candidate, uint32_t expectedGeneration) {
  if (!candidate.validSemantics()) return false;
  if (gConfigTransactionMutex &&
      xSemaphoreTake(gConfigTransactionMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    return false;
  const bool result = configCommitInternal(candidate, expectedGeneration);
  if (gConfigTransactionMutex) xSemaphoreGive(gConfigTransactionMutex);
  return result;
}

uint32_t configGeneration() { return gConfigGeneration.load(std::memory_order_acquire); }

void configLoad() { gConfig.load(); }

bool configApplyTransaction(const RuntimeConfig& candidate, uint32_t expectedGeneration,
                            const std::function<bool()>& apply,
                            const std::function<bool()>& rollbackRuntime) {
  if (!gConfigTransactionMutex ||
      xSemaphoreTake(gConfigTransactionMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    return false;
  auto unlock = [&]() { xSemaphoreGive(gConfigTransactionMutex); };

  RuntimeConfig previous;
  uint32_t previousGeneration = 0;
  if (!candidate.validSemantics()) { unlock(); return false; }
  if (!configSnapshot(previous, previousGeneration) || previousGeneration != expectedGeneration) { unlock(); return false; }

  const uint32_t candidateGeneration =
      expectedGeneration == UINT32_MAX ? 1U : expectedGeneration + 1U;

  Preferences journal;
  if (!journal.begin(NVS_NS, false)) { unlock(); return false; }
  bool journalOk =
      journal.putUInt(NVS_TXN_PREV_GEN, previousGeneration) == sizeof(uint32_t) &&
      journal.putUInt(NVS_TXN_CANDIDATE_GEN, candidateGeneration) == sizeof(uint32_t) &&
      journal.putUChar(NVS_TXN_STATE, CONFIG_TXN_PENDING) == sizeof(uint8_t);
  journal.end();
  if (!journalOk) { unlock(); return false; }
  // ENH-2: Record durable transaction journal creation.
  configTxnAudit(ConfigTxnEvent::Pending, previousGeneration, "journal");

  if (!configCommitInternal(candidate, expectedGeneration)) {
    Preferences clear;
    if (clear.begin(NVS_NS, false)) {
      (void)clear.remove(NVS_TXN_STATE);
      (void)clear.remove(NVS_TXN_PREV_GEN);
      (void)clear.remove(NVS_TXN_CANDIDATE_GEN);
      clear.end();
    }
    configTxnAudit(ConfigTxnEvent::JournalCleared, previousGeneration, "commit-failed");
    unlock();
    return false;
  }
  // ENH-2: Record successful persisted commit.
  configTxnAudit(ConfigTxnEvent::Committed, candidateGeneration, "configCommitInternal");

  // ENH-2: Record the apply invocation immediately before calling apply().
  if (apply) configTxnAudit(ConfigTxnEvent::Applied, candidateGeneration, "invoking");
  if (apply && apply()) {
    Preferences committed;
    const bool committedOpen = committed.begin(NVS_NS, false);
    if (!committedOpen ||
        committed.putUChar(NVS_TXN_STATE, CONFIG_TXN_COMMITTED) != sizeof(uint8_t)) {
      if (committedOpen) committed.end();
      const bool runtimeRollback = rollbackRuntime ? rollbackRuntime() : true;
      const bool persistedRollback = configCommitInternal(previous, configGeneration());
      const bool rolledBack = runtimeRollback && persistedRollback;
      if (rolledBack) configTxnAudit(ConfigTxnEvent::RolledBack, previousGeneration, "commit-marker");
      unlock();
      return rolledBack;
    }
    committed.end();

    Preferences clear;
    if (clear.begin(NVS_NS, false)) {
      (void)clear.remove(NVS_TXN_STATE);
      (void)clear.remove(NVS_TXN_PREV_GEN);
      (void)clear.remove(NVS_TXN_CANDIDATE_GEN);
      clear.end();
      configTxnAudit(ConfigTxnEvent::JournalCleared, candidateGeneration, "committed");
    }
    unlock();
    return true;
  }

  // ENH-2: Record failed runtime application before rollback.
  configTxnAudit(ConfigTxnEvent::ApplyFailed, candidateGeneration, "apply");
  const bool runtimeRollback = rollbackRuntime ? rollbackRuntime() : true;
  const bool persistedRollback = configCommitInternal(previous, configGeneration());
  Preferences clear;
  if (clear.begin(NVS_NS, false)) {
    (void)clear.remove(NVS_TXN_STATE);
    (void)clear.remove(NVS_TXN_PREV_GEN);
    (void)clear.remove(NVS_TXN_CANDIDATE_GEN);
    clear.end();
  }
  const bool result = runtimeRollback && persistedRollback;
  // ENH-2: Record completed runtime+persistent rollback.
  if (result) configTxnAudit(ConfigTxnEvent::RolledBack, previousGeneration, "apply-failed");
  configTxnAudit(ConfigTxnEvent::JournalCleared, previousGeneration, "rolled-back");
  unlock();
  return result;
}

bool configManagerBegin() {
  if (configQueue) return true;
  if (!gConfigMutex) return false;
  if (!gConfigTransactionMutex) {
    gConfigTransactionMutex = xSemaphoreCreateMutex();
    if (!gConfigTransactionMutex) return false;
  }
  configQueue = xQueueCreateStatic(CONFIG_COMMAND_QUEUE_DEPTH, sizeof(ConfigCommand), configQueueStorage, &configQueueStruct);
  if (!configQueue) return false;
  if (xTaskCreate(configManagerTask, "ConfigMgr", 6144, nullptr, 2, &configTaskHandle) != pdPASS) {
    configQueue = nullptr; configTaskHandle = nullptr; return false;
  }
  return true;
}

namespace {
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

bool validEstCredential(const String& value, size_t maxLen) {
  if (!validCredential(value, maxLen)) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool validHexKey(const String& value);
bool validCallsign(const String& value);

constexpr uint32_t ATOMIC_CONFIG_MAGIC = 0x43464732UL;
constexpr uint16_t ATOMIC_CONFIG_SCHEMA = Config::ATOMIC_CONFIG_SCHEMA_VERSION;
constexpr uint8_t ATOMIC_CONFIG_COMMIT = 0xA5;
constexpr char NVS_SLOT_A[] = "cfgslot_a";
constexpr char NVS_SLOT_B[] = "cfgslot_b";
constexpr char NVS_COMMIT_A[] = "cfgcommit_a";
constexpr char NVS_COMMIT_B[] = "cfgcommit_b";

struct __attribute__((packed)) PersistedConfigPayload {
  float loraFreqMHz; float loraBwKHz; uint8_t loraSf; uint8_t loraCr; uint8_t loraSyncWord; int8_t loraPowerDbm;
  uint8_t volume; uint8_t audioRecordSource; float batteryCalibration;
  char callsign[17]; char loraKeyHex[33]; char apSsid[33]; char apPassword[64]; char webUser[33];
  char webPasswordSaltHex[33]; char webPasswordHashHex[65]; uint8_t audioRecordQuality;
  uint8_t lorawanEnabled; uint8_t lorawanMode; uint8_t lorawanRegion; char lorawanDevEui[17]; char lorawanJoinEui[17];
  char lorawanAppKey[33]; char lorawanNwkSKey[33]; char lorawanAppSKey[33]; uint8_t lorawanDevAddr[4]; uint8_t lorawanFPort; uint16_t lorawanUplinkPeriodSec;
  uint8_t blePairingEnabled; uint8_t mqttEnabled; uint32_t wakePeriodSec; uint8_t classDEnabled; uint8_t classDBoostLevel;
  uint8_t deepSleepEnabled; uint32_t deepSleepIdleMs; uint32_t deepSleepWakeGraceMs; uint32_t criticalShutdownDelayMs;
  float batteryLowThreshold; float batteryCriticalThreshold; char mqttHost[254]; uint16_t mqttPort; uint8_t mqttTlsRequired;
  uint32_t mqttReconnectMinMs; uint32_t mqttReconnectMaxMs; uint32_t mqttTelemetryPeriodMs; uint32_t mqttHealthPeriodMs;
  uint8_t mqttRetainTelemetry; uint8_t mqttRetainAvailability; uint16_t mqttCredentialRotationDays;
  uint8_t voxEnabled; float voxThreshold; uint32_t voxHangMs; uint8_t aecEnabled; uint8_t usbMonitor; uint8_t usbPlaybackTransport; uint8_t audioLoopback;
  uint8_t loraAdrEnabled; uint8_t loraHopEnabled; uint8_t loraHopChannelProfile; uint8_t loraRangeTestMode;
  uint8_t sensorReaderEnabled; uint32_t sensorScanIntervalMs; uint16_t sensorScanWindowMs; uint32_t sensorScanDurationMs; uint32_t sensorConnectTimeoutMs;
  uint32_t sensorNodeEvictionMs; uint8_t sensorMaxNodes; uint8_t sensorRequireEncryption; uint8_t blePairingFailureThreshold; uint32_t blePairingBlockMs; uint8_t sensorKeepAwake;
  uint32_t webSessionTimeoutMs; uint32_t webAuthRateLimitMs; uint8_t csrfPolicy; uint8_t blePairingPolicy; uint8_t ecdhRekeyPolicy; uint8_t replayWindowBits;
  char estServerUrl[254]; char estLabel[96]; uint16_t certRenewalThresholdDays; uint32_t certCheckPeriodMs;
  uint8_t estAuthMode; char estUsername[65]; char estPassword[65]; char estBootstrapToken[129]; uint8_t estBootstrapTokenConsumed; uint8_t certLifecycleEnabled;
  char staSsid[33]; char staPassword[64];
};
struct __attribute__((packed)) AtomicConfigRecord { uint32_t magic; uint16_t schema; uint16_t payloadSize; uint32_t generation; PersistedConfigPayload payload; uint32_t crc; };
static_assert(sizeof(AtomicConfigRecord) < 4096, "atomic config must fit in one NVS blob");

constexpr uint16_t ATOMIC_CONFIG_SCHEMA_V2 = 2;
constexpr size_t ATOMIC_CONFIG_SCHEMA_V2_TAIL_BYTES = sizeof(char[33]) + sizeof(char[64]);
constexpr size_t ATOMIC_CONFIG_SCHEMA_V2_PAYLOAD_SIZE =
    sizeof(PersistedConfigPayload) - ATOMIC_CONFIG_SCHEMA_V2_TAIL_BYTES;
struct __attribute__((packed)) AtomicConfigRecordV2 {
  uint32_t magic;
  uint16_t schema;
  uint16_t payloadSize;
  uint32_t generation;
  uint8_t payload[ATOMIC_CONFIG_SCHEMA_V2_PAYLOAD_SIZE];
  uint32_t crc;
};
static_assert(sizeof(AtomicConfigRecordV2) < 4096, "legacy atomic config must fit in one NVS blob");

uint32_t atomicCrc32(const uint8_t* data, size_t len) {
  uint32_t crc=0xFFFFFFFFUL; for(size_t i=0;i<len;++i){ crc^=data[i]; for(uint8_t b=0;b<8;++b) crc=(crc&1U)?(crc>>1U)^0xEDB88320UL:crc>>1U; } return ~crc;
}
void putStr(char* dst,size_t cap,const String& v){ if(!dst||!cap)return; size_t n=min(v.length(),cap-1U); memcpy(dst,v.c_str(),n); dst[n]='\0'; }
String getStr(const char* src,size_t cap){ if(!src||!cap)return String(); size_t n=0; while(n<cap&&src[n])++n; return n==cap?String():String(src); }

void encodePayload(const RuntimeConfig& c, PersistedConfigPayload& p) {
  memset(&p,0,sizeof(p)); p.loraFreqMHz=c.loraFreqMHz;p.loraBwKHz=c.loraBwKHz;p.loraSf=c.loraSf;p.loraCr=c.loraCr;p.loraSyncWord=c.loraSyncWord;p.loraPowerDbm=c.loraPowerDbm;p.volume=c.volume;p.audioRecordSource=c.audioRecordSource;p.batteryCalibration=c.batteryCalibration;
  putStr(p.callsign,sizeof(p.callsign),c.callsign);putStr(p.loraKeyHex,sizeof(p.loraKeyHex),c.loraKeyHex);putStr(p.apSsid,sizeof(p.apSsid),c.apSsid);putStr(p.apPassword,sizeof(p.apPassword),c.apPassword);putStr(p.webUser,sizeof(p.webUser),c.webUser);putStr(p.webPasswordSaltHex,sizeof(p.webPasswordSaltHex),c.webPasswordSaltHex);putStr(p.webPasswordHashHex,sizeof(p.webPasswordHashHex),c.webPasswordHashHex);
  p.audioRecordQuality=c.audioRecordQuality;p.lorawanEnabled=c.lorawanEnabled;p.lorawanMode=c.lorawanMode;p.lorawanRegion=c.lorawanRegion;putStr(p.lorawanDevEui,sizeof(p.lorawanDevEui),c.lorawanDevEui);putStr(p.lorawanJoinEui,sizeof(p.lorawanJoinEui),c.lorawanJoinEui);putStr(p.lorawanAppKey,sizeof(p.lorawanAppKey),c.lorawanAppKey);putStr(p.lorawanNwkSKey,sizeof(p.lorawanNwkSKey),c.lorawanNwkSKey);putStr(p.lorawanAppSKey,sizeof(p.lorawanAppSKey),c.lorawanAppSKey);memcpy(p.lorawanDevAddr,c.lorawanDevAddr,4);p.lorawanFPort=c.lorawanFPort;p.lorawanUplinkPeriodSec=c.lorawanUplinkPeriodSec;
  p.blePairingEnabled=c.blePairingEnabled;p.mqttEnabled=c.mqttEnabled;p.wakePeriodSec=c.wakePeriodSec;p.classDEnabled=c.classDEnabled;p.classDBoostLevel=c.classDBoostLevel;p.deepSleepEnabled=c.deepSleepEnabled;p.deepSleepIdleMs=c.deepSleepIdleMs;p.deepSleepWakeGraceMs=c.deepSleepWakeGraceMs;p.criticalShutdownDelayMs=c.criticalShutdownDelayMs;p.batteryLowThreshold=c.batteryLowThreshold;p.batteryCriticalThreshold=c.batteryCriticalThreshold;putStr(p.mqttHost,sizeof(p.mqttHost),c.mqttHost);p.mqttPort=c.mqttPort;p.mqttTlsRequired=c.mqttTlsRequired;p.mqttReconnectMinMs=c.mqttReconnectMinMs;p.mqttReconnectMaxMs=c.mqttReconnectMaxMs;p.mqttTelemetryPeriodMs=c.mqttTelemetryPeriodMs;p.mqttHealthPeriodMs=c.mqttHealthPeriodMs;p.mqttRetainTelemetry=c.mqttRetainTelemetry;p.mqttRetainAvailability=c.mqttRetainAvailability;p.mqttCredentialRotationDays=c.mqttCredentialRotationDays;
  p.voxEnabled=c.voxEnabled;p.voxThreshold=c.voxThreshold;p.voxHangMs=c.voxHangMs;p.aecEnabled=c.aecEnabled;p.usbMonitor=c.usbMonitor;p.usbPlaybackTransport=c.usbPlaybackTransport;p.audioLoopback=c.audioLoopback;p.loraAdrEnabled=c.loraAdrEnabled;p.loraHopEnabled=c.loraHopEnabled;p.loraHopChannelProfile=c.loraHopChannelProfile;p.loraRangeTestMode=c.loraRangeTestMode;
  p.sensorReaderEnabled=c.sensorReaderEnabled;p.sensorScanIntervalMs=c.sensorScanIntervalMs;p.sensorScanWindowMs=c.sensorScanWindowMs;p.sensorScanDurationMs=c.sensorScanDurationMs;p.sensorConnectTimeoutMs=c.sensorConnectTimeoutMs;p.sensorNodeEvictionMs=c.sensorNodeEvictionMs;p.sensorMaxNodes=c.sensorMaxNodes;p.sensorRequireEncryption=c.sensorRequireEncryption;p.blePairingFailureThreshold=c.blePairingFailureThreshold;p.blePairingBlockMs=c.blePairingBlockMs;p.sensorKeepAwake=c.sensorKeepAwake;p.webSessionTimeoutMs=c.webSessionTimeoutMs;p.webAuthRateLimitMs=c.webAuthRateLimitMs;p.csrfPolicy=c.csrfPolicy;p.blePairingPolicy=c.blePairingPolicy;p.ecdhRekeyPolicy=c.ecdhRekeyPolicy;p.replayWindowBits=c.replayWindowBits;
  putStr(p.estServerUrl,sizeof(p.estServerUrl),c.estServerUrl); putStr(p.estLabel,sizeof(p.estLabel),c.estLabel);
  p.certRenewalThresholdDays=c.certRenewalThresholdDays; p.certCheckPeriodMs=c.certCheckPeriodMs;
  p.estAuthMode=c.estAuthMode; putStr(p.estUsername,sizeof(p.estUsername),c.estUsername);
  putStr(p.estPassword,sizeof(p.estPassword),c.estPassword);
  putStr(p.estBootstrapToken,sizeof(p.estBootstrapToken),c.estBootstrapToken);
  p.estBootstrapTokenConsumed=c.estBootstrapTokenConsumed;
  p.certLifecycleEnabled=c.certLifecycleEnabled;
  putStr(p.staSsid,sizeof(p.staSsid),c.staSsid);
  putStr(p.staPassword,sizeof(p.staPassword),c.staPassword);
}

bool decodePayload(const PersistedConfigPayload& p, RuntimeConfig& c) {
  c.loraFreqMHz=p.loraFreqMHz;c.loraBwKHz=p.loraBwKHz;c.loraSf=p.loraSf;c.loraCr=p.loraCr;c.loraSyncWord=p.loraSyncWord;c.loraPowerDbm=p.loraPowerDbm;c.volume=p.volume;c.audioRecordSource=p.audioRecordSource;c.batteryCalibration=p.batteryCalibration;c.callsign=getStr(p.callsign,sizeof(p.callsign));c.loraKeyHex=getStr(p.loraKeyHex,sizeof(p.loraKeyHex));c.apSsid=getStr(p.apSsid,sizeof(p.apSsid));c.apPassword=getStr(p.apPassword,sizeof(p.apPassword));c.webUser=getStr(p.webUser,sizeof(p.webUser));c.webPassword.clear();c.webPasswordSaltHex=getStr(p.webPasswordSaltHex,sizeof(p.webPasswordSaltHex));c.webPasswordHashHex=getStr(p.webPasswordHashHex,sizeof(p.webPasswordHashHex));c.audioRecordQuality=p.audioRecordQuality;c.lorawanEnabled=p.lorawanEnabled!=0;c.lorawanMode=p.lorawanMode;c.lorawanRegion=p.lorawanRegion;c.lorawanDevEui=getStr(p.lorawanDevEui,sizeof(p.lorawanDevEui));c.lorawanJoinEui=getStr(p.lorawanJoinEui,sizeof(p.lorawanJoinEui));c.lorawanAppKey=getStr(p.lorawanAppKey,sizeof(p.lorawanAppKey));c.lorawanNwkSKey=getStr(p.lorawanNwkSKey,sizeof(p.lorawanNwkSKey));c.lorawanAppSKey=getStr(p.lorawanAppSKey,sizeof(p.lorawanAppSKey));memcpy(c.lorawanDevAddr,p.lorawanDevAddr,4);c.lorawanFPort=p.lorawanFPort;c.lorawanUplinkPeriodSec=p.lorawanUplinkPeriodSec;c.blePairingEnabled=p.blePairingEnabled!=0;c.mqttEnabled=p.mqttEnabled!=0;c.wakePeriodSec=p.wakePeriodSec;c.classDEnabled=p.classDEnabled!=0;c.classDBoostLevel=p.classDBoostLevel;c.deepSleepEnabled=p.deepSleepEnabled!=0;c.deepSleepIdleMs=p.deepSleepIdleMs;c.deepSleepWakeGraceMs=p.deepSleepWakeGraceMs;c.criticalShutdownDelayMs=p.criticalShutdownDelayMs;c.batteryLowThreshold=p.batteryLowThreshold;c.batteryCriticalThreshold=p.batteryCriticalThreshold;c.mqttHost=getStr(p.mqttHost,sizeof(p.mqttHost));c.mqttPort=p.mqttPort;c.mqttTlsRequired=p.mqttTlsRequired!=0;c.mqttReconnectMinMs=p.mqttReconnectMinMs;c.mqttReconnectMaxMs=p.mqttReconnectMaxMs;c.mqttTelemetryPeriodMs=p.mqttTelemetryPeriodMs;c.mqttHealthPeriodMs=p.mqttHealthPeriodMs;c.mqttRetainTelemetry=p.mqttRetainTelemetry!=0;c.mqttRetainAvailability=p.mqttRetainAvailability!=0;c.mqttCredentialRotationDays=p.mqttCredentialRotationDays;c.voxEnabled=p.voxEnabled!=0;c.voxThreshold=p.voxThreshold;c.voxHangMs=p.voxHangMs;c.aecEnabled=p.aecEnabled!=0;c.usbMonitor=p.usbMonitor!=0;c.usbPlaybackTransport=p.usbPlaybackTransport!=0;c.audioLoopback=p.audioLoopback!=0;c.loraAdrEnabled=p.loraAdrEnabled!=0;c.loraHopEnabled=p.loraHopEnabled!=0;c.loraHopChannelProfile=p.loraHopChannelProfile;c.loraRangeTestMode=p.loraRangeTestMode!=0;c.sensorReaderEnabled=p.sensorReaderEnabled!=0;c.sensorScanIntervalMs=p.sensorScanIntervalMs;c.sensorScanWindowMs=p.sensorScanWindowMs;c.sensorScanDurationMs=p.sensorScanDurationMs;c.sensorConnectTimeoutMs=p.sensorConnectTimeoutMs;c.sensorNodeEvictionMs=p.sensorNodeEvictionMs;c.sensorMaxNodes=p.sensorMaxNodes;c.sensorRequireEncryption=p.sensorRequireEncryption!=0;c.blePairingFailureThreshold=p.blePairingFailureThreshold;c.blePairingBlockMs=p.blePairingBlockMs;c.sensorKeepAwake=p.sensorKeepAwake!=0;c.webSessionTimeoutMs=p.webSessionTimeoutMs;c.webAuthRateLimitMs=p.webAuthRateLimitMs;c.csrfPolicy=p.csrfPolicy;c.blePairingPolicy=p.blePairingPolicy;c.ecdhRekeyPolicy=p.ecdhRekeyPolicy;c.replayWindowBits=p.replayWindowBits;
  c.estServerUrl=getStr(p.estServerUrl,sizeof(p.estServerUrl)); c.estLabel=getStr(p.estLabel,sizeof(p.estLabel));
  c.certRenewalThresholdDays=p.certRenewalThresholdDays; c.certCheckPeriodMs=p.certCheckPeriodMs;
  c.estAuthMode=p.estAuthMode; c.estUsername=getStr(p.estUsername,sizeof(p.estUsername));
  c.estPassword=getStr(p.estPassword,sizeof(p.estPassword));
  c.estBootstrapToken=getStr(p.estBootstrapToken,sizeof(p.estBootstrapToken));
  c.estBootstrapTokenConsumed=p.estBootstrapTokenConsumed!=0;
  c.certLifecycleEnabled=p.certLifecycleEnabled!=0;c.staSsid=getStr(p.staSsid,sizeof(p.staSsid));c.staPassword=getStr(p.staPassword,sizeof(p.staPassword)); return true;
}

bool generationNewer(uint32_t a,uint32_t b){return a!=b&&static_cast<int32_t>(a-b)>0;}
bool readAtomicSlot(Preferences& p,const char* slot,const char* commit,AtomicConfigRecord& r){memset(&r,0,sizeof(r));if(p.getUChar(commit,0)!=ATOMIC_CONFIG_COMMIT)return false;if(p.getBytes(slot,&r,sizeof(r))!=sizeof(r))return false;return r.magic==ATOMIC_CONFIG_MAGIC&&r.schema==ATOMIC_CONFIG_SCHEMA&&r.payloadSize==sizeof(r.payload)&&r.generation!=0&&r.crc==atomicCrc32(reinterpret_cast<const uint8_t*>(&r),offsetof(AtomicConfigRecord,crc));}
bool readAtomicSlotV2(Preferences& p,const char* slot,const char* commit,PersistedConfigPayload& payload,uint32_t& generation){
  AtomicConfigRecordV2 r{};
  if(p.getUChar(commit,0)!=ATOMIC_CONFIG_COMMIT) return false;
  if(p.getBytes(slot,&r,sizeof(r))!=sizeof(r)) return false;
  if(r.magic!=ATOMIC_CONFIG_MAGIC || r.schema!=ATOMIC_CONFIG_SCHEMA_V2 ||
     r.payloadSize!=sizeof(r.payload) || r.generation==0 ||
     r.crc!=atomicCrc32(reinterpret_cast<const uint8_t*>(&r),offsetof(AtomicConfigRecordV2,crc))) return false;
  memset(&payload,0,sizeof(payload));
  memcpy(&payload,r.payload,sizeof(r.payload));
  generation=r.generation;
  return true;
}

bool validRuntimeConfig(const RuntimeConfig& c) {
  return c.validRadio()&&c.volume<=100&&c.audioRecordSource<=Config::AUDIO_SOURCE_USB&&c.audioRecordQuality<=2&&c.classDBoostLevel<=7&&c.wakePeriodSec>=Config::WAKE_PERIOD_SEC_MIN&&c.wakePeriodSec<=Config::WAKE_PERIOD_SEC_MAX&&(!c.classDEnabled||Config::CLASS_D_ENABLED)&&c.deepSleepIdleMs>=Config::DEEP_SLEEP_IDLE_MS_MIN&&c.deepSleepIdleMs<=Config::DEEP_SLEEP_IDLE_MS_MAX&&c.deepSleepWakeGraceMs>=100UL&&c.deepSleepWakeGraceMs<=60000UL&&c.criticalShutdownDelayMs>=100UL&&c.criticalShutdownDelayMs<=600000UL&&isfinite(c.batteryLowThreshold)&&isfinite(c.batteryCriticalThreshold)&&c.batteryCriticalThreshold>=Config::BATTERY_CRITICAL_THRESHOLD_MIN&&c.batteryLowThreshold>c.batteryCriticalThreshold&&c.batteryLowThreshold<=Config::BATTERY_LOW_THRESHOLD_MAX&&(!c.mqttEnabled||(!c.mqttHost.isEmpty()&&c.mqttHost.length()<=253&&c.mqttHost.indexOf('|')<0&&c.mqttPort!=0))&&c.mqttReconnectMinMs>=Config::MQTT_RECONNECT_MS_MIN&&c.mqttReconnectMaxMs>=c.mqttReconnectMinMs&&c.mqttReconnectMaxMs<=Config::MQTT_RECONNECT_MS_MAX&&c.mqttTelemetryPeriodMs>=Config::MQTT_TELEMETRY_PERIOD_MS_MIN&&c.mqttTelemetryPeriodMs<=Config::MQTT_TELEMETRY_PERIOD_MS_MAX&&c.mqttHealthPeriodMs>=1000UL&&c.mqttHealthPeriodMs<=86400000UL&&c.mqttCredentialRotationDays>=1&&c.mqttCredentialRotationDays<=3650&&(!c.mqttEnabled||!Config::mqttTlsIsMandatory()||c.mqttTlsRequired)&&c.voxThreshold>=0.005f&&c.voxThreshold<=1.0f&&c.voxHangMs>=50U&&c.voxHangMs<=10000U&&c.loraHopChannelProfile>=1&&c.loraHopChannelProfile<=Config::HOP_CHANNEL_MAX&&c.sensorScanIntervalMs>=100&&c.sensorScanIntervalMs<=60000&&c.sensorScanWindowMs>0&&c.sensorScanWindowMs<=c.sensorScanIntervalMs&&c.sensorScanDurationMs>=100&&c.sensorScanDurationMs<=60000&&c.sensorConnectTimeoutMs>=500&&c.sensorConnectTimeoutMs<=30000&&c.sensorNodeEvictionMs>=10000&&c.sensorNodeEvictionMs<=7UL*86400000UL&&c.sensorMaxNodes>=1&&c.sensorMaxNodes<=Config::SENSOR_MAX_NODES_VALUE&&c.blePairingFailureThreshold>=1&&c.blePairingFailureThreshold<=20&&c.blePairingBlockMs>=1000&&c.blePairingBlockMs<=86400000UL&&c.webSessionTimeoutMs>=Config::WEB_SESSION_TIMEOUT_MS_MIN&&c.webSessionTimeoutMs<=Config::WEB_SESSION_TIMEOUT_MS_MAX&&c.webAuthRateLimitMs>=Config::WEB_AUTH_RATE_LIMIT_MS_MIN&&c.webAuthRateLimitMs<=Config::WEB_AUTH_RATE_LIMIT_MS_MAX&&c.csrfPolicy<=1&&c.blePairingPolicy<=1&&c.ecdhRekeyPolicy<=1&&c.replayWindowBits>=Config::LORA_REPLAY_WINDOW_BITS_MIN&&c.replayWindowBits<=Config::LORA_REPLAY_WINDOW_BITS&&
    c.estServerUrl.length()<=253&&c.estLabel.length()<=95&&!c.estLabel.isEmpty()&&
    c.estAuthMode<=2&&c.estUsername.length()<=64&&c.estPassword.length()<=64&&c.estBootstrapToken.length()<=128&&
    (c.estAuthMode!=1 || (validEstCredential(c.estUsername,64) && validEstCredential(c.estPassword,64)))&&
    (c.estAuthMode!=2 || validEstCredential(c.estBootstrapToken,128) || c.estBootstrapTokenConsumed)&&
    c.staSsid.length()<=Config::STA_SSID_MAX_LEN &&
    ((c.staSsid.isEmpty() && c.staPassword.isEmpty()) ||
     (!c.staSsid.isEmpty() && c.staPassword.length()>=Config::STA_PASSWORD_MIN_LEN &&
      c.staPassword.length()<=Config::STA_PASSWORD_MAX_LEN)) &&
    c.certRenewalThresholdDays>=Config::CERT_RENEWAL_THRESHOLD_DAYS_MIN&&c.certRenewalThresholdDays<=Config::CERT_RENEWAL_THRESHOLD_DAYS_MAX&&
    c.certCheckPeriodMs>=3600000UL&&c.certCheckPeriodMs<=7UL*86400000UL&&
    (!c.certLifecycleEnabled || (c.estServerUrl.startsWith("https://") && c.estLabel.startsWith("/")))&&
    isfinite(c.batteryCalibration)&&c.batteryCalibration>=0.5f&&c.batteryCalibration<=1.5f&&c.validLoRaWAN()&&validCallsign(c.callsign)&&validHexKey(c.loraKeyHex)&&!c.apSsid.isEmpty()&&c.apSsid.length()<=32&&c.apPassword.length()>=8&&c.apPassword.length()<=63&&!c.webUser.isEmpty()&&c.webUser.length()<=32&&c.webPasswordConfigured();
}

bool saveAtomicConfig(const RuntimeConfig& source);
bool validRuntimeConfig(const RuntimeConfig& c);

constexpr uint32_t MRAM_CONFIG_MAGIC = 0x4D434647UL; // "MCFG"
constexpr uint16_t MRAM_CONFIG_SCHEMA = 1;
constexpr uint32_t MRAM_MIGRATION_MAGIC = 0x314D524DUL; // "MRM1"

struct __attribute__((packed)) MramConfigRecord {
  uint32_t magic;
  uint16_t schema;
  uint16_t payloadSize;
  uint32_t generation;
  PersistedConfigPayload payload;
  uint32_t crc;
};
struct __attribute__((packed)) MramMigrationMarker {
  uint32_t magic;
  uint16_t schema;
  uint16_t reserved;
  uint32_t generation;
  uint32_t crc;
};
static_assert(sizeof(MramConfigRecord) <= Config::PERSISTENT_CONFIG_MRAM_SLOT_BYTES,
              "MRAM PersistentConfig record must fit in 4KB slot");

void stripMramCredentials(PersistedConfigPayload& p) {
  memset(p.loraKeyHex, 0, sizeof(p.loraKeyHex));
  memset(p.apPassword, 0, sizeof(p.apPassword));
  memset(p.webPasswordSaltHex, 0, sizeof(p.webPasswordSaltHex));
  memset(p.webPasswordHashHex, 0, sizeof(p.webPasswordHashHex));
  memset(p.lorawanAppKey, 0, sizeof(p.lorawanAppKey));
  memset(p.lorawanNwkSKey, 0, sizeof(p.lorawanNwkSKey));
  memset(p.lorawanAppSKey, 0, sizeof(p.lorawanAppSKey));
  memset(p.estUsername, 0, sizeof(p.estUsername));
  memset(p.estPassword, 0, sizeof(p.estPassword));
  memset(p.estBootstrapToken, 0, sizeof(p.estBootstrapToken));
  memset(p.staPassword, 0, sizeof(p.staPassword));
}

bool readMramMarker(MramStorage& mram, MramMigrationMarker& marker) {
  if (!mram.read(Config::PERSISTENT_CONFIG_MRAM_MARKER, &marker, sizeof(marker))) return false;
  return marker.magic == MRAM_MIGRATION_MAGIC && marker.schema == MRAM_CONFIG_SCHEMA &&
         marker.generation != 0 &&
         marker.crc == atomicCrc32(reinterpret_cast<const uint8_t*>(&marker), offsetof(MramMigrationMarker, crc));
}

bool readMramConfigSlot(MramStorage& mram, uint16_t slot, uint16_t commit, MramConfigRecord& record) {
  memset(&record, 0, sizeof(record));
  uint8_t committed = 0;
  if (!mram.read(commit, &committed, sizeof(committed)) || committed != Config::PERSISTENT_CONFIG_MRAM_COMMIT) return false;
  if (!mram.read(slot, &record, sizeof(record))) return false;
  return record.magic == MRAM_CONFIG_MAGIC && record.schema == MRAM_CONFIG_SCHEMA &&
         record.payloadSize == sizeof(record.payload) && record.generation != 0 &&
         record.crc == atomicCrc32(reinterpret_cast<const uint8_t*>(&record), offsetof(MramConfigRecord, crc));
}

bool loadNvsCredentialFields(RuntimeConfig& out, uint32_t expectedGeneration = 0) {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;
  out.loraKeyHex = p.getString("lorakey", out.loraKeyHex);
  out.apPassword = p.getString("appass", out.apPassword);
  out.webUser = p.getString("webuser", out.webUser);
  out.webPasswordSaltHex = p.getString("websalt", out.webPasswordSaltHex);
  out.webPasswordHashHex = p.getString("webph", out.webPasswordHashHex);
  out.lorawanAppKey = p.getString("lw_appkey", out.lorawanAppKey);
  out.lorawanNwkSKey = p.getString("lw_nwkskey", out.lorawanNwkSKey);
  out.lorawanAppSKey = p.getString("lw_appskey", out.lorawanAppSKey);
  out.estUsername = p.getString("est_user", out.estUsername);
  out.estPassword = p.getString("est_pass", out.estPassword);
  out.estBootstrapToken = p.getString("est_token", out.estBootstrapToken);
  out.staPassword = p.getString("sta_pass", out.staPassword);
  // The atomic NVS record remains the encrypted credential source on devices
  // where the credentials are stored there rather than in individual keys.
  AtomicConfigRecord a{}, b{};
  const bool va = readAtomicSlot(p, NVS_SLOT_A, NVS_COMMIT_A, a);
  const bool vb = readAtomicSlot(p, NVS_SLOT_B, NVS_COMMIT_B, b);
  if (va || vb) {
    const AtomicConfigRecord* r = va && vb ? (generationNewer(a.generation, b.generation) ? &a : &b) : (va ? &a : &b);
    if (expectedGeneration != 0 && r->generation != expectedGeneration) {
      p.end();
      return false;
    }
    RuntimeConfig credentials = out;
    if (!decodePayload(r->payload, credentials)) {
      if (expectedGeneration != 0) {
        p.end();
        return false;
      }
    } else {
      out.loraKeyHex = credentials.loraKeyHex; out.apPassword = credentials.apPassword;
      out.webUser = credentials.webUser; out.webPasswordSaltHex = credentials.webPasswordSaltHex;
      out.webPasswordHashHex = credentials.webPasswordHashHex; out.lorawanAppKey = credentials.lorawanAppKey;
      out.lorawanNwkSKey = credentials.lorawanNwkSKey; out.lorawanAppSKey = credentials.lorawanAppSKey;
      out.estUsername = credentials.estUsername; out.estPassword = credentials.estPassword;
      out.estBootstrapToken = credentials.estBootstrapToken; out.staPassword = credentials.staPassword;
    }
  } else if (expectedGeneration != 0) {
    // Once MRAM is authoritative, legacy per-key credentials are not a valid
    // source: accepting them would mix generations after an interrupted save.
    p.end();
    return false;
  }
  p.end();
  return true;
}

bool loadMramConfig(RuntimeConfig& out, uint32_t& generation, bool& authoritative) {
  authoritative = false; generation = 0;
  MramStorage& mram = MramStorage::shared();
  if (!mram.begin()) return false;
  MramMigrationMarker marker{};
  if (!readMramMarker(mram, marker)) return false;
  authoritative = true;
  generation = marker.generation;
  MramConfigRecord a{}, b{};
  const bool va = readMramConfigSlot(mram, Config::PERSISTENT_CONFIG_MRAM_SLOT_A, Config::PERSISTENT_CONFIG_MRAM_COMMIT_A, a);
  const bool vb = readMramConfigSlot(mram, Config::PERSISTENT_CONFIG_MRAM_SLOT_B, Config::PERSISTENT_CONFIG_MRAM_COMMIT_B, b);
  if (!va && !vb) return false;
  const MramConfigRecord* r = va && vb ? (generationNewer(a.generation, b.generation) ? &a : &b) : (va ? &a : &b);
  if (!decodePayload(r->payload, out) || !validRuntimeConfig(out)) return false;
  if (!loadNvsCredentialFields(out, r->generation) || !validRuntimeConfig(out)) return false;
  generation = r->generation;
  return true;
}

bool mramAuthorityMarkerPresent() {
  MramStorage& mram = MramStorage::shared();
  if (!mram.begin()) return false;
  MramMigrationMarker marker{};
  return readMramMarker(mram, marker);
}

bool saveMramConfig(const RuntimeConfig& source, uint32_t& generation, uint32_t requestedGeneration = 0) {
  MramStorage& mram = MramStorage::shared();
  if (!mram.begin()) return false;
  MramMigrationMarker existingMarker{};
  const bool markerAlreadyValid = readMramMarker(mram, existingMarker);
  MramConfigRecord a{}, b{};
  const bool va = readMramConfigSlot(mram, Config::PERSISTENT_CONFIG_MRAM_SLOT_A, Config::PERSISTENT_CONFIG_MRAM_COMMIT_A, a);
  const bool vb = readMramConfigSlot(mram, Config::PERSISTENT_CONFIG_MRAM_SLOT_B, Config::PERSISTENT_CONFIG_MRAM_COMMIT_B, b);
  uint32_t current = 0;
  bool writeA = true;
  if (va && (!vb || generationNewer(a.generation, b.generation))) { current = a.generation; writeA = false; }
  else if (vb) { current = b.generation; writeA = true; }
  const uint32_t next = requestedGeneration ? requestedGeneration : (current == UINT32_MAX ? 1U : current + 1U);
  MramConfigRecord record{};
  record.magic = MRAM_CONFIG_MAGIC; record.schema = MRAM_CONFIG_SCHEMA;
  record.payloadSize = sizeof(record.payload); record.generation = next;
  encodePayload(source, record.payload); stripMramCredentials(record.payload);
  record.crc = atomicCrc32(reinterpret_cast<const uint8_t*>(&record), offsetof(MramConfigRecord, crc));
  const uint16_t slot = writeA ? Config::PERSISTENT_CONFIG_MRAM_SLOT_A : Config::PERSISTENT_CONFIG_MRAM_SLOT_B;
  const uint16_t commit = writeA ? Config::PERSISTENT_CONFIG_MRAM_COMMIT_A : Config::PERSISTENT_CONFIG_MRAM_COMMIT_B;
  uint8_t clear = 0;
  if (!mram.write(commit, &clear, sizeof(clear))) return false;
  if (!mram.write(slot, &record, sizeof(record))) return false;
  MramConfigRecord verify{};
  if (!mram.read(slot, &verify, sizeof(verify)) || memcmp(&verify, &record, sizeof(record)) != 0) return false;
  const uint8_t committed = Config::PERSISTENT_CONFIG_MRAM_COMMIT;
  if (!mram.write(commit, &committed, sizeof(committed))) return false;
  MramConfigRecord committedRecord{};
  if (!readMramConfigSlot(mram, slot, commit, committedRecord) || committedRecord.generation != next) return false;
  if (!markerAlreadyValid) {
    MramMigrationMarker marker{};
    marker.magic = MRAM_MIGRATION_MAGIC; marker.schema = MRAM_CONFIG_SCHEMA; marker.generation = next;
    marker.crc = atomicCrc32(reinterpret_cast<const uint8_t*>(&marker), offsetof(MramMigrationMarker, crc));
    if (!mram.write(Config::PERSISTENT_CONFIG_MRAM_MARKER, &marker, sizeof(marker))) return false;
  }
  generation = next;
  return true;
}

bool loadAtomicConfig(RuntimeConfig& out,uint32_t& generation,bool* legacySchema=nullptr){
  if (legacySchema) *legacySchema = false;
  Preferences p;
  if(!p.begin(NVS_NS,true)) return false;
  AtomicConfigRecord a{},b{};
  bool va=readAtomicSlot(p,NVS_SLOT_A,NVS_COMMIT_A,a),vb=readAtomicSlot(p,NVS_SLOT_B,NVS_COMMIT_B,b);
  if(!va&&!vb){
    PersistedConfigPayload legacyPayloadA{}, legacyPayloadB{};
    uint32_t legacyGenerationA=0, legacyGenerationB=0;
    const bool lva=readAtomicSlotV2(p,NVS_SLOT_A,NVS_COMMIT_A,legacyPayloadA,legacyGenerationA);
    const bool lvb=readAtomicSlotV2(p,NVS_SLOT_B,NVS_COMMIT_B,legacyPayloadB,legacyGenerationB);
    if(!lva&&!lvb){p.end();return false;}
    const PersistedConfigPayload& legacyPayload =
        lva&&(!lvb||generationNewer(legacyGenerationA,legacyGenerationB)) ? legacyPayloadA : legacyPayloadB;
    RuntimeConfig candidate=out;
    if(!decodePayload(legacyPayload,candidate)) {p.end();return false;}
    candidate.staSsid = Config::STA_SSID;
    candidate.staPassword = Config::STA_PASSWORD;
    if(!validRuntimeConfig(candidate)){p.end();return false;}
    out=candidate;
    generation=lva&&(!lvb||generationNewer(legacyGenerationA,legacyGenerationB)) ? legacyGenerationA : legacyGenerationB;
    if (legacySchema) *legacySchema = true;
    p.end();
    return true;
  }
  const AtomicConfigRecord* newest = va&&(!vb||generationNewer(a.generation,b.generation)) ? &a : &b;
  const AtomicConfigRecord* chosen = newest;
  const bool pending = p.getUChar(NVS_TXN_STATE, 0) == CONFIG_TXN_PENDING;
  const uint32_t candidateGeneration = p.getUInt(NVS_TXN_CANDIDATE_GEN, 0);
  const uint32_t previousGeneration = p.getUInt(NVS_TXN_PREV_GEN, 0);
  if (pending && newest->generation == candidateGeneration && previousGeneration != 0) {
    if (va && vb) {
      const AtomicConfigRecord* previous =
          (a.generation == previousGeneration) ? &a :
          (b.generation == previousGeneration) ? &b : nullptr;
      if (previous) chosen = previous;
    }
  }
  RuntimeConfig candidate=out;
  if(!decodePayload(chosen->payload,candidate)||!candidate.validSemantics()){p.end();return false;}
  out=candidate;generation=chosen->generation;p.end();return true;
}

bool saveAtomicConfig(const RuntimeConfig& source){Preferences p;if(!p.begin(NVS_NS,false))return false;AtomicConfigRecord a{},b{};bool va=readAtomicSlot(p,NVS_SLOT_A,NVS_COMMIT_A,a),vb=readAtomicSlot(p,NVS_SLOT_B,NVS_COMMIT_B,b);uint32_t current=0;bool writeA=true;if(va&&(!vb||generationNewer(a.generation,b.generation))){current=a.generation;writeA=false;}else if(vb){current=b.generation;writeA=true;}uint32_t next=current==UINT32_MAX?1U:current+1U;AtomicConfigRecord r{};r.magic=ATOMIC_CONFIG_MAGIC;r.schema=ATOMIC_CONFIG_SCHEMA;r.payloadSize=sizeof(r.payload);r.generation=next;encodePayload(source,r.payload);r.crc=atomicCrc32(reinterpret_cast<const uint8_t*>(&r),offsetof(AtomicConfigRecord,crc));const char* sk=writeA?NVS_SLOT_A:NVS_SLOT_B;const char* ck=writeA?NVS_COMMIT_A:NVS_COMMIT_B;(void)p.remove(ck);if(p.putBytes(sk,&r,sizeof(r))!=sizeof(r)){p.end();return false;}AtomicConfigRecord verify{};if(p.getBytes(sk,&verify,sizeof(verify))!=sizeof(verify)||memcmp(&verify,&r,sizeof(r))!=0){p.end();return false;}if(p.putUChar(ck,ATOMIC_CONFIG_COMMIT)!=sizeof(uint8_t)){p.end();return false;}AtomicConfigRecord committed{};bool ok=readAtomicSlot(p,sk,ck,committed)&&committed.generation==next&&memcmp(&committed,&r,sizeof(r))==0;p.end();if(!ok)return false;
  uint32_t mramGeneration = 0;
  if (!saveMramConfig(source, mramGeneration, next)) {
    Preferences rollback;
    if (rollback.begin(NVS_NS, false)) {
      (void)rollback.remove(ck);
      rollback.end();
    }
    return false;
  }
  gConfigGeneration.store(next,std::memory_order_release);Preferences legacy;if(legacy.begin(NVS_NS,false)){(void)legacy.remove("webpass");legacy.end();}return true;}


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

bool RuntimeConfig::validSemantics() const {
  return validRuntimeConfig(*this);
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
  // MRAM is the runtime backend. NVS remains the migration/credential source.
  uint32_t mramGeneration = 0;
  bool mramAuthoritative = false;
  RuntimeConfig mramCandidate = *this;
  if (loadMramConfig(mramCandidate, mramGeneration, mramAuthoritative)) {
    *this = mramCandidate;
    gConfigGeneration.store(mramGeneration, std::memory_order_release);
    Preferences recovery;
    if (recovery.begin(NVS_NS, false)) {
      (void)recovery.remove(NVS_TXN_STATE);
      (void)recovery.remove(NVS_TXN_PREV_GEN);
      (void)recovery.remove(NVS_TXN_CANDIDATE_GEN);
      recovery.end();
    }
    return;
  } else if (mramAuthoritative) {
    // The MRM1 marker proves that MRAM became authoritative, but a torn/corrupt
    // config record must not brick the device when an equal-or-newer complete
    // NVS generation is available. Recovery is allowed only when it cannot
    // downgrade below the last authoritative marker generation.
    RuntimeConfig recoveryCandidate = *this;
    uint32_t nvsGeneration = 0;
    bool legacySchema = false;
    if (loadAtomicConfig(recoveryCandidate, nvsGeneration, &legacySchema) &&
        (nvsGeneration == mramGeneration || generationNewer(nvsGeneration, mramGeneration)) &&
        validRuntimeConfig(recoveryCandidate)) {
      uint32_t recoveredGeneration = 0;
      if (saveMramConfig(recoveryCandidate, recoveredGeneration, nvsGeneration)) {
        *this = recoveryCandidate;
        gConfigGeneration.store(recoveredGeneration, std::memory_order_release);
        Serial.println("CONFIG RECOVERY: restored MRAM from equal-or-newer NVS generation");
        return;
      }
    }
    Serial.println("CONFIG: MRAM is authoritative and no safe equal-or-newer recovery exists");
    return;
  } else {
    uint32_t nvsGeneration = 0; bool legacySchema = false;
    RuntimeConfig nvsCandidate = *this;
    if (loadAtomicConfig(nvsCandidate, nvsGeneration, &legacySchema)) {
      uint32_t migratedGeneration = 0;
      if (!saveMramConfig(nvsCandidate, migratedGeneration, nvsGeneration)) {
        Serial.println("CONFIG MIGRATION: NVS -> MRAM failed; retaining NVS-loaded runtime state");
      } else {
        *this = nvsCandidate;
        gConfigGeneration.store(migratedGeneration, std::memory_order_release);
        mramAuthoritative = true;
        Serial.println("CONFIG MIGRATION: NVS -> MRAM committed");
      }
    }
  }

  // ENH-2: Observe a pending journal before atomic load so recovery can be audited.
  uint32_t pendingPreviousGeneration = 0;
  uint32_t pendingCandidateGeneration = 0;
  bool hadPendingTransaction = false;
  {
    Preferences recoveryProbe;
    if (recoveryProbe.begin(NVS_NS, true)) {
      hadPendingTransaction =
          recoveryProbe.getUChar(NVS_TXN_STATE, 0) == CONFIG_TXN_PENDING;
      pendingPreviousGeneration = recoveryProbe.getUInt(NVS_TXN_PREV_GEN, 0);
      pendingCandidateGeneration = recoveryProbe.getUInt(NVS_TXN_CANDIDATE_GEN, 0);
      recoveryProbe.end();
    }
  }

  uint32_t atomicGeneration = 0;
  RuntimeConfig atomicCandidate = *this;
  bool migratedLegacyAtomic = false;
  if (!mramAuthoritative && loadAtomicConfig(atomicCandidate, atomicGeneration, &migratedLegacyAtomic)) {
    bool migratedMqttTls = false;
    if (migratedLegacyAtomic) {
      // Schema-2 atomic payloads predate persistent STA credentials. The
      // compile-time defaults above are retained until the complete schema-3
      // record is committed below.
      if (saveAtomicConfig(atomicCandidate)) {
        atomicGeneration = gConfigGeneration.load(std::memory_order_acquire);
      } else {
        Serial.println("CONFIG MIGRATION: failed to upgrade atomic schema 2 -> 3");
      }
    }
    if (Config::mqttTlsIsMandatory() && !atomicCandidate.mqttTlsRequired) {
      atomicCandidate.mqttTlsRequired = true;
      migratedMqttTls = true;
      if (saveAtomicConfig(atomicCandidate)) {
        atomicGeneration = gConfigGeneration.load(std::memory_order_acquire);
      } else {
        Serial.println("CONFIG MIGRATION: failed to persist mqtt_tls=true; runtime remains TLS-only");
      }
    }
    *this = atomicCandidate;
    gConfigGeneration.store(atomicGeneration, std::memory_order_release);
    if (migratedMqttTls) {
      Serial.println("CONFIG MIGRATION: mqtt_tls false -> true (production TLS mandatory)");
    }
    if (hadPendingTransaction &&
        atomicGeneration == pendingPreviousGeneration &&
        pendingCandidateGeneration != pendingPreviousGeneration) {
      configTxnAudit(ConfigTxnEvent::Recovered, atomicGeneration, "pending->previous");
    }
    Preferences recovery;
    if (recovery.begin(NVS_NS, false)) {
      (void)recovery.remove(NVS_TXN_STATE);
      (void)recovery.remove(NVS_TXN_PREV_GEN);
      (void)recovery.remove(NVS_TXN_CANDIDATE_GEN);
      recovery.end();
      configTxnAudit(ConfigTxnEvent::JournalCleared, atomicGeneration, "boot");
    }
    return;
  }
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
  const String savedEstUrl = prefs.getString("est_url", estServerUrl);
  const String savedEstLabel = prefs.getString("est_label", estLabel);
  const uint16_t savedCertThreshold = prefs.getUShort("cert_thr", certRenewalThresholdDays);
  const uint32_t savedCertPeriod = prefs.getUInt("cert_period", certCheckPeriodMs);
  const uint8_t savedEstAuth = prefs.getUChar("est_auth", estAuthMode);
  const String savedEstUsername = prefs.getString("est_user", estUsername);
  const String savedEstPassword = prefs.getString("est_pass", estPassword);
  const String savedEstToken = prefs.getString("est_token", estBootstrapToken);
  const bool savedEstTokenConsumed = prefs.getBool("est_token_used", estBootstrapTokenConsumed);
  const bool savedCertLifecycle = prefs.getBool("cert_life", certLifecycleEnabled);
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
  const bool migratedMqttTls = Config::mqttTlsIsMandatory() && !candidate.mqttTlsRequired;
  if (migratedMqttTls) candidate.mqttTlsRequired = true;
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
  candidate.estServerUrl = savedEstUrl;
  candidate.estLabel = savedEstLabel;
  candidate.certRenewalThresholdDays = savedCertThreshold;
  candidate.certCheckPeriodMs = savedCertPeriod;
  candidate.estAuthMode = savedEstAuth;
  candidate.estUsername = savedEstUsername;
  candidate.estPassword = savedEstPassword;
  candidate.estBootstrapToken = savedEstToken;
  candidate.estBootstrapTokenConsumed = savedEstTokenConsumed;
  candidate.certLifecycleEnabled = savedCertLifecycle;
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
  if (candidate.wakePeriodSec >= Config::WAKE_PERIOD_SEC_MIN && candidate.wakePeriodSec <= Config::WAKE_PERIOD_SEC_MAX)
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
  if (candidate.mqttTelemetryPeriodMs >= Config::MQTT_TELEMETRY_PERIOD_MS_MIN && candidate.mqttTelemetryPeriodMs <= Config::MQTT_TELEMETRY_PERIOD_MS_MAX)
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
  if (candidate.webSessionTimeoutMs >= Config::WEB_SESSION_TIMEOUT_MS_MIN && candidate.webSessionTimeoutMs <= Config::WEB_SESSION_TIMEOUT_MS_MAX)
    webSessionTimeoutMs = candidate.webSessionTimeoutMs;
  if (candidate.webAuthRateLimitMs >= Config::WEB_AUTH_RATE_LIMIT_MS_MIN && candidate.webAuthRateLimitMs <= Config::WEB_AUTH_RATE_LIMIT_MS_MAX)
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
  if (candidate.estServerUrl.length() <= 253 && candidate.estServerUrl.indexOf('|') < 0)
    estServerUrl = candidate.estServerUrl;
  if (candidate.estLabel.length() <= 95 && candidate.estLabel.startsWith("/"))
    estLabel = candidate.estLabel;
  if (candidate.certRenewalThresholdDays >= Config::CERT_RENEWAL_THRESHOLD_DAYS_MIN && candidate.certRenewalThresholdDays <= Config::CERT_RENEWAL_THRESHOLD_DAYS_MAX)
    certRenewalThresholdDays = candidate.certRenewalThresholdDays;
  if (candidate.certCheckPeriodMs >= 3600000UL && candidate.certCheckPeriodMs <= 7UL * 86400000UL)
    certCheckPeriodMs = candidate.certCheckPeriodMs;
  if (candidate.estAuthMode <= 2) {
    estAuthMode = candidate.estAuthMode;
    if (candidate.estUsername.length() <= 64) estUsername = candidate.estUsername;
    if (candidate.estPassword.length() <= 64) estPassword = candidate.estPassword;
    if (candidate.estBootstrapToken.length() <= 128) {
      if (candidate.estBootstrapToken != estBootstrapToken) estBootstrapTokenConsumed = false;
      estBootstrapToken = candidate.estBootstrapToken;
    }
    if (candidate.estBootstrapTokenConsumed) estBootstrapTokenConsumed = true;
  }
  certLifecycleEnabled = candidate.certLifecycleEnabled &&
                         candidate.estServerUrl.startsWith("https://") &&
                         (candidate.estAuthMode == 0 ||
                          (candidate.estAuthMode == 1 && !candidate.estUsername.isEmpty() && !candidate.estPassword.isEmpty()) ||
                          (candidate.estAuthMode == 2 && (!candidate.estBootstrapToken.isEmpty() || candidate.estBootstrapTokenConsumed)));
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
  const bool needsSave = !webPassword.isEmpty() ||
                         version == 4 ||
                         (version != CONFIG_VERSION && webPasswordConfigured()) ||
                         migratedMqttTls;
  if (needsSave) {
    const bool saved = save();
    if (migratedMqttTls) {
      Serial.println(saved
          ? "CONFIG MIGRATION: mqtt_tls false -> true (production TLS mandatory)"
          : "CONFIG MIGRATION: mqtt_tls false -> true; persistence failed, runtime remains TLS-only");
    }
  }
}

bool RuntimeConfig::migrate() {
  load();
  return save();
}

bool RuntimeConfig::save() {
  if (!validRuntimeConfig(*this)) return false;
  if (!webPassword.isEmpty()) {
    uint8_t salt[PASSWORD_SALT_BYTES] = {}; uint8_t hash[32] = {};
    for (size_t i=0;i<sizeof(salt);i+=4) { uint32_t r=esp_random(); memcpy(salt+i,&r,min<size_t>(4,sizeof(salt)-i)); }
    if (!passwordHash(webPassword,salt,hash)) { mbedtls_platform_zeroize(salt,sizeof(salt)); mbedtls_platform_zeroize(hash,sizeof(hash)); return false; }
    webPasswordSaltHex=hexEncode(salt,sizeof(salt)); webPasswordHashHex=hexEncode(hash,sizeof(hash));
    mbedtls_platform_zeroize(salt,sizeof(salt)); mbedtls_platform_zeroize(hash,sizeof(hash));
  }
  if (!webPasswordConfigured()) return false;
  return saveAtomicConfig(*this);
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
