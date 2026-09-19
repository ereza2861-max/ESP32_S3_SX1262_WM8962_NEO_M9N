#include "LoRaManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "PersistentConfig.h"
#include "AudioManager.h"
#include "Telemetry.h"
#include "StorageManager.h"
#include "RadioArbiter.h"
#include "SensorTelemetry.h"
#include "EncryptedFrameParser.h"
#include <esp_system.h>
#include <Preferences.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>
#include <time.h>
#include <esp_attr.h>
#include <SD.h>
#include <math.h>

extern AudioManager audio;
extern StorageManager storage;

namespace {
constexpr uint8_t PACKET_MAGIC = 0xF1;
constexpr size_t PACKET_HEADER_V1 = 1 + 1 + 1 + 2 + 4;
constexpr size_t PACKET_HEADER_V2 = PACKET_HEADER_V1 + 4 + 1;
constexpr size_t PACKET_HEADER_V3 = PACKET_HEADER_V2 + 1 + sizeof(uint32_t);
constexpr size_t PACKET_HEADER_V4 = PACKET_HEADER_V2 + 8; // V2 nonce 4B -> V4 nonce 12B
constexpr size_t PACKET_HEADER_V5 = Config::LORA_ECDH_V5_HEADER_BYTES;
static_assert(PACKET_HEADER_V5 == PACKET_HEADER_V3 + 1,
              "V5 header must be V3 header plus key_epoch_delta");
constexpr size_t PACKET_HEADER_TX =
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
    PACKET_HEADER_V5;
#else
    PACKET_HEADER_V2;
#endif
constexpr uint8_t ROUTE_EXT_MAGIC = 0xE7;
constexpr uint8_t ROUTE_EXT_VERSION_V1 = 1;
constexpr uint8_t ROUTE_EXT_VERSION = 2;
constexpr uint8_t ROUTE_EXT_FLAG_BROADCAST = 0x01;
constexpr uint8_t ROUTE_EXT_MAX_HOPS = Config::LORA_INITIAL_TTL;
constexpr uint32_t ROUTE_CACHE_TTL_MS = 120000UL;
constexpr uint16_t FRAG_STORE_MAGIC = 0x4651;
constexpr uint8_t FRAG_STORE_VERSION = 2;
constexpr uint16_t ROUTE_ETX_MAX_Q8 = 0x7FFF;
constexpr uint8_t LORA_PROTOCOL_VERSION_HOP = 3;
constexpr size_t PACKET_TAG = Config::LORA_TAG_BYTES;
constexpr uint8_t FRAGMENT_MAGIC = 0xF2;
constexpr uint8_t BEACON_MAGIC = 0xB1;
constexpr uint8_t VOICE_ACK_MAGIC = 0xA5;
constexpr uint8_t VOICE_ACK_VERSION = 1;
constexpr uint8_t VOICE_ACK_BYTES = 16;
constexpr uint8_t VOICE_ACK_LEGACY_BYTES = 12;
constexpr uint8_t TX_PRIORITY_SOS = 100;
constexpr uint8_t TX_PRIORITY_ACK = 90;
constexpr uint8_t TX_PRIORITY_VOICE = 80;
constexpr uint8_t TX_PRIORITY_TEXT = 60;
constexpr uint8_t TX_PRIORITY_BEACON = 20;
constexpr uint8_t TX_PRIORITY_FORWARD = 10;
constexpr char FORWARD_QUEUE_FILE[] = "/LORA/FWD.Q";
constexpr char FORWARD_QUEUE_BACKUP_FILE[] = "/LORA/FWD.BAK";
constexpr uint16_t FORWARD_RECORD_MAGIC = 0x4C51;
constexpr size_t FORWARD_RECORD_FIXED_V1 = 2 + 1 + 1 + 1 + 1 + 2 + 4 + 2 + 4;
constexpr size_t FORWARD_RECORD_FIXED = FORWARD_RECORD_FIXED_V1 + 1;
constexpr uint8_t FORWARD_RECORD_VERSION = 2;

struct RtcRadioState {
  uint32_t magic;
  uint32_t hopFrame;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  uint32_t ecdhKeyEpoch;
  bool ecdhActive;
#endif
  uint16_t sosSeq;
  uint8_t sosRetryCount;
  bool sosAwaitingAck;
  uint32_t sosElapsedMs;
  uint16_t sosPacketLen;
  uint8_t sosPacket[Config::LORA_MAX_PACKET];
  uint32_t crc;
};
RTC_DATA_ATTR RtcRadioState rtcRadioState{};
constexpr uint32_t RTC_RADIO_MAGIC = 0x46525231UL;

uint32_t stateCrc(const RtcRadioState& st) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&st);
  uint32_t crc = 2166136261UL;
  for (size_t i = 0; i < offsetof(RtcRadioState, crc); ++i) {
    crc ^= p[i];
    crc *= 16777619UL;
  }
  return crc;
}

uint32_t currentEpochSec() {
  const time_t now = time(nullptr);
  return now > 1700000000 && now < 4102444800 ? static_cast<uint32_t>(now) : 0;
}

bool deriveRotatingKey(const uint8_t master[16], uint32_t epochSec, uint8_t out[16]) {
  if (!master || !out || epochSec == 0) return false;
  const uint32_t period = epochSec / Config::LORA_REKEY_PERIOD_SEC;
  uint8_t msg[12] = {'F','R','-','R','E','K','E','Y', static_cast<uint8_t>(period), static_cast<uint8_t>(period >> 8), static_cast<uint8_t>(period >> 16), static_cast<uint8_t>(period >> 24)};
  unsigned char digest[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, master, 16, msg, sizeof(msg), digest, sizeof(digest)) != 0) return false;
  memcpy(out, digest, 16);
  return true;
}

bool hexByte(const char* p, uint8_t& out) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  const int hi = nibble(p[0]), lo = nibble(p[1]);
  if (hi < 0 || lo < 0) return false;
  out = static_cast<uint8_t>((hi << 4) | lo);
  return true;
}
}

LoRaManager* LoRaManager::instance_ = nullptr;

LoRaManager::LoRaManager()
    : module_(Board::LORA_CS, Board::LORA_DIO1, Board::LORA_RST, Board::LORA_BUSY),
      radio_(&module_) {}

void LoRaManager::onDio1() {
  if (instance_) {
    // RadioLib invokes this callback from the radio interrupt path. Keep it
    // ISR-safe: only increment a volatile counter; do not touch String/RTOS.
    portENTER_CRITICAL_ISR(&instance_->irqMux_);
    if (instance_->irqCount_ != UINT32_MAX) ++instance_->irqCount_;
    portEXIT_CRITICAL_ISR(&instance_->irqMux_);
  }
}

void LoRaManager::refillDutyBudget() {
  portENTER_CRITICAL(&dutyMux_);
  const uint32_t now = millis();
  if (lastDutyRefillMs_ == 0) {
    lastDutyRefillMs_ = now;
    dutyTokensUs_ = (static_cast<uint64_t>(Config::LORA_DUTY_WINDOW_MS) *
                     Config::LORA_DUTY_CYCLE_PERCENT * 1000ULL) / 100ULL;
    portEXIT_CRITICAL(&dutyMux_);
    return;
  }

  const uint32_t elapsedMs = now - lastDutyRefillMs_;
  if (!elapsedMs) {
    portEXIT_CRITICAL(&dutyMux_);
    return;
  }

  const uint64_t maxBudget = dutyMaxBudgetUs();
  const uint64_t refill =
      (static_cast<uint64_t>(elapsedMs) * Config::LORA_DUTY_CYCLE_PERCENT *
       1000ULL) / 100ULL;
  dutyTokensUs_ = min(maxBudget, dutyTokensUs_ + refill);
  lastDutyRefillMs_ = now;
  portEXIT_CRITICAL(&dutyMux_);
}

bool LoRaManager::consumeDutyBudget(uint32_t airtimeUs) {
  if (airtimeUs == 0) return false;
  portENTER_CRITICAL(&dutyMux_);
  const uint32_t now = millis();
  if (lastDutyRefillMs_ == 0) {
    lastDutyRefillMs_ = now;
    dutyTokensUs_ = dutyMaxBudgetUs();
  } else {
    const uint32_t elapsedMs = now - lastDutyRefillMs_;
    if (elapsedMs) {
      const uint64_t maxBudget = dutyMaxBudgetUs();
      const uint64_t refill =
          (static_cast<uint64_t>(elapsedMs) * Config::LORA_DUTY_CYCLE_PERCENT *
           1000ULL) / 100ULL;
      dutyTokensUs_ = min(maxBudget, dutyTokensUs_ + refill);
      lastDutyRefillMs_ = now;
    }
  }
  const bool ok = dutyTokensUs_ >= airtimeUs;
  if (ok) dutyTokensUs_ -= airtimeUs;
  portEXIT_CRITICAL(&dutyMux_);
  return ok;
}

void LoRaManager::refundDutyBudget(uint32_t airtimeUs) {
  if (airtimeUs == 0) return;
  portENTER_CRITICAL(&dutyMux_);
  const uint64_t maxBudget = dutyMaxBudgetUs();
  const uint64_t before = dutyTokensUs_;
  dutyTokensUs_ = min(maxBudget, dutyTokensUs_ + static_cast<uint64_t>(airtimeUs));
  const bool clamped = before + static_cast<uint64_t>(airtimeUs) > maxBudget;
  portEXIT_CRITICAL(&dutyMux_);
  if (clamped) Serial.println("WARN: LoRa duty budget refund clamped");
}

uint64_t LoRaManager::dutyMaxBudgetUs() const {
  return (static_cast<uint64_t>(Config::LORA_DUTY_WINDOW_MS) *
          Config::LORA_DUTY_CYCLE_PERCENT * 1000ULL) / 100ULL;
}

uint32_t LoRaManager::forwardQueued() const {
  return forwardQueue_ ? static_cast<uint32_t>(uxQueueMessagesWaiting(forwardQueue_)) : 0;
}

bool LoRaManager::loadKey(uint8_t key[16]) const {
  if (!key || gConfig.loraKeyHex.length() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    if (!hexByte(gConfig.loraKeyHex.c_str() + i * 2, key[i])) return false;
  }
  return true;
}

void LoRaManager::hardResetRadio() const {
  if (Board::LORA_RST < 0) return;
  pinMode(Board::LORA_RST, OUTPUT);
  digitalWrite(Board::LORA_RST, LOW);
  delay(10);
  digitalWrite(Board::LORA_RST, HIGH);
  delay(20);
}

bool LoRaManager::reserveTxSequenceBlock() {
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) return false;

  const uint32_t storedHighWater = prefs.getUInt("txseq_hi", 0);
  if (storedHighWater > UINT32_MAX - Config::LORA_TX_SEQUENCE_RESERVATION) {
    prefs.end();
    return false;
  }
  const uint32_t newHighWater = storedHighWater + Config::LORA_TX_SEQUENCE_RESERVATION;
  if (prefs.putUInt("txseq_hi", newHighWater) != sizeof(uint32_t)) {
    prefs.end();
    return false;
  }
  prefs.end();

  txSequenceAbsolute_ = static_cast<uint32_t>(storedHighWater) + 1U;
  txSequenceReservedUntil_ = newHighWater;
  txSequence_ = static_cast<uint16_t>(storedHighWater & 0xFFFFU);
  return true;
}

bool LoRaManager::nextTxSequence(uint16_t& seq) {
  if (!seqMutex_ || xSemaphoreTake(seqMutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;

  bool ok = txSequenceAbsolute_ != 0;
  if (ok && txSequenceAbsolute_ > txSequenceReservedUntil_)
    ok = reserveTxSequenceBlock();

  if (ok) {
    seq = static_cast<uint16_t>(txSequenceAbsolute_ & 0xFFFFU);
    ++txSequenceAbsolute_;
    txSequence_ = seq;
  }

  xSemaphoreGive(seqMutex_);
  return ok;
}

int8_t LoRaManager::effectiveTxPowerDbm() const {
  int8_t configured = gConfig.loraPowerDbm;
  {
    StateLock lock(gState);
    if (lock.ok() && gState.brownoutReset && millis() < 60000UL)
      configured = min<int8_t>(configured, static_cast<int8_t>(Config::BATTERY_TX_POWER_LOW_DBM));
  }
  float battery = NAN;
  bool low = false, critical = false;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      battery = gState.batteryV;
      low = gState.batteryLow;
      critical = gState.batteryCritical;
    }
  }
  if (!isfinite(battery)) return configured;
  if (critical) {
    const int8_t limit = static_cast<int8_t>(Config::BATTERY_TX_POWER_CRITICAL_DBM);
    return configured < limit ? configured : limit;
  }
  if (low) {
    const int8_t limit = static_cast<int8_t>(Config::BATTERY_TX_POWER_LOW_DBM);
    return configured < limit ? configured : limit;
  }
  return configured;
}

bool LoRaManager::acceptReplay(uint32_t sourceId, uint16_t seq, uint8_t type, uint32_t payloadHash, uint32_t packetEpochSec) {
  const uint32_t now = millis();
  const uint32_t currentEpoch = currentEpochSec();
  if (packetEpochSec != 0 && currentEpoch != 0) {
    // Reject both stale and implausibly future frames. An absolute-difference
    // check alone would allow a forged future timestamp within the window.
    if (packetEpochSec > currentEpoch) {
      if (packetEpochSec - currentEpoch > Config::LORA_REPLAY_TIME_WINDOW_SEC) {
        ++replayRejects_;
        return true;
      }
    } else if (currentEpoch - packetEpochSec > Config::LORA_REPLAY_TIME_WINDOW_SEC) {
      ++replayRejects_;
      return true;
    }
  }
  ReplayEntry* slot = nullptr;
  for (auto& entry : replayCache_) {
    if (entry.sourceId == sourceId && entry.type == type) {
      slot = &entry;
      break;
    }
  }
  if (!slot) {
    slot = &replayCache_[replayNext_];
    replayNext_ = (replayNext_ + 1) % Config::LORA_REPLAY_SOURCE_CACHE_SIZE;
    slot->sourceId = sourceId;
    slot->type = type;
    slot->highestSeq = seq;
    slot->bitmap = 1U;
    slot->highestPayloadHash = payloadHash;
    slot->seenMs = now;
    slot->lastEpochSec = packetEpochSec;
    (void)persistReplayEntry(*slot);
    return false;
  }

  // RFC1982-style serial-number arithmetic: valid forward movement is less
  // than half the 16-bit sequence space, including across 0xFFFF -> 0x0000.
  const uint16_t delta = static_cast<uint16_t>(seq - slot->highestSeq);
  if (delta != 0 && delta < 0x8000U) {
    const uint8_t shift = static_cast<uint8_t>(
        min<uint16_t>(delta, Config::LORA_REPLAY_WINDOW_BITS));
    slot->bitmap = shift >= 32 ? 1U : (slot->bitmap << shift) | 1U;
    slot->highestSeq = seq;
    slot->highestPayloadHash = payloadHash;
    slot->seenMs = now;
    slot->lastEpochSec = packetEpochSec;
    return false;
  }

  // A sequence number identifies one authenticated origin frame. Forwarding
  // may legitimately rewrite the routing extension, but a node must not accept
  // the same origin sequence repeatedly just because the routed payload hash
  // changed; doing so permits replaying alternate authenticated route variants.
  if (delta == 0) {
    ++replayRejects_;
    return true;
  }

  const uint16_t age = static_cast<uint16_t>(slot->highestSeq - seq);
  if (age >= Config::LORA_REPLAY_WINDOW_BITS &&
      age < 0x8000U) {
    ++replayRejects_;
    return true;
  }
  if (age >= 0x8000U) {
    ++replayRejects_;
    return true;
  }
  const uint8_t clampedAge = static_cast<uint8_t>(
      min<uint16_t>(age, Config::LORA_REPLAY_WINDOW_BITS - 1U));
  const uint32_t bit = 1UL << clampedAge;
  if (slot->bitmap & bit) {
    ++replayRejects_;
    return true;
  }
  slot->bitmap |= bit;
  slot->highestPayloadHash = payloadHash;
  slot->seenMs = now;
  slot->lastEpochSec = packetEpochSec;
  (void)persistReplayEntry(*slot);
  return false;
}

bool LoRaManager::persistReplayEntry(const ReplayEntry& entry) {
  size_t index = Config::LORA_REPLAY_SOURCE_CACHE_SIZE;
  for (size_t i = 0; i < Config::LORA_REPLAY_SOURCE_CACHE_SIZE; ++i) {
    if (&replayCache_[i] == &entry) { index = i; break; }
  }
  if (index == Config::LORA_REPLAY_SOURCE_CACHE_SIZE) {
    for (size_t i = 0; i < Config::LORA_REPLAY_SOURCE_CACHE_SIZE; ++i) {
      if (replayCache_[i].sourceId == entry.sourceId &&
          replayCache_[i].type == entry.type) {
        index = i; break;
      }
    }
  }
  if (index == Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;

  if (replayStore_.persist(entry, index)) return true;
  // The NVS fallback deliberately compacts before its 64-record journal is
  // exhausted. This snapshot is infrequent and preserves all replay slots.
  if (Config::REPLAY_STORE_BACKEND == Config::ReplayStoreBackend::BACKEND_NVS_JOURNAL)
    return replayStore_.flushAll(replayCache_, Config::LORA_REPLAY_SOURCE_CACHE_SIZE);
  return false;
}

bool LoRaManager::loadReplayState() {
  memset(replayCache_, 0, sizeof(replayCache_));
  replayNext_ = 0;
  replayStateLoaded_ = false;
  if (!replayStore_.healthy() ||
      !replayStore_.load(replayCache_, Config::LORA_REPLAY_SOURCE_CACHE_SIZE))
    return false;
  for (size_t i = 0; i < Config::LORA_REPLAY_SOURCE_CACHE_SIZE; ++i) {
    if (replayCache_[i].sourceId != 0) {
      replayNext_ = (i + 1U) % Config::LORA_REPLAY_SOURCE_CACHE_SIZE;
    }
  }
  replayStateLoaded_ = true;
  return true;
}

void LoRaManager::updateAntennaHealthAfterTx() {
  float forwardDbm = 0.0f;
  float reflectedDbm = 0.0f;
  float vswr = Config::MAX2016_VSWR_MAX;
  const int16_t fallbackRssi = static_cast<int16_t>(radio_.getRSSI());
  if (rfDetector_.healthy() &&
      rfDetector_.read(forwardDbm, reflectedDbm, vswr)) {
    const int16_t txPower = static_cast<int16_t>(lroundf(forwardDbm));
    StateLock lock(gState);
    if (lock.ok()) {
      gState.txRssi = txPower;
      if (gState.antennaBaselineRssi <= -127)
        gState.antennaBaselineRssi = txPower;
      gState.antennaOk = isfinite(vswr) && vswr < Config::MAX2016_ANTENNA_OK_VSWR;
    }
    (void)reflectedDbm;
    return;
  }

  StateLock lock(gState);
  if (!lock.ok()) return;
  gState.lastError = "MAX2016 detector unhealthy; using LoRa RSSI fallback";
  gState.txRssi = fallbackRssi;
  if (gState.antennaBaselineRssi <= -127) {
    gState.antennaBaselineRssi = fallbackRssi;
    gState.antennaOk = true;
  } else {
    gState.antennaOk =
        abs(static_cast<int>(fallbackRssi) -
            static_cast<int>(gState.antennaBaselineRssi)) >= 3;
  }
}

uint16_t LoRaManager::crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b)
      crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
  }
  return crc;
}

bool LoRaManager::encryptPacket(const uint8_t* plain, size_t len, uint8_t type,
                                uint16_t seq, String& packet) {
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  if (type != Config::LORA_TYPE_NEIGHBOR_BEACON) {
    // DECISION: under flag=1 every data packet is pairwise V5. The peer is
    // resolved from the authenticated route envelope; broadcast without a
    // concrete peer has no ECDH session and is rejected rather than downgraded.
    uint32_t peerSourceId = 0;
    if (plain && len >= 12 && plain[0] == ROUTE_EXT_MAGIC &&
        (plain[1] == ROUTE_EXT_VERSION ||
         plain[1] == ROUTE_EXT_VERSION_V1)) {
      uint32_t destination = 0;
      uint32_t nextHop = 0;
      memcpy(&destination, plain + 4, sizeof(destination));
      memcpy(&nextHop, plain + 8, sizeof(nextHop));
      peerSourceId = nextHop != 0 ? nextHop : destination;
    }
    if (peerSourceId == 0) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "ECDH peer unsupported";
      return false;
    }
    const uint32_t epochSec =
        currentEpochSec() != 0 ? currentEpochSec() : ecdhAuthoritativeEpochSec_;
    if (epochSec == 0)
      return false;
    return encryptPacketV5(plain, len, type, seq,
                           computeHopIndex(hopFrame_), epochSec,
                           peerSourceId,
                           Config::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT,
                           packet);
  }
#endif
  uint8_t key[16];
  if (!plain || !loadKey(key))
    return false;

  if (Config::LORA_USE_AES_GCM) {
    if (len + PACKET_HEADER_V4 + PACKET_TAG > Config::LORA_MAX_PACKET)
      return false;

    uint8_t nonce[12] = {};
    for (size_t i = 0; i < sizeof(nonce); i += sizeof(uint32_t)) {
      const uint32_t randomWord = esp_random();
      memcpy(nonce + i, &randomWord,
             min(sizeof(randomWord), sizeof(nonce) - i));
    }

    const uint8_t ttl = Config::LORA_INITIAL_TTL;
    packet.reserve(PACKET_HEADER_V4 + len + PACKET_TAG);
    packet += static_cast<char>(PACKET_MAGIC);
    packet += static_cast<char>(Config::LORA_PROTOCOL_VERSION_GCM);
    packet += static_cast<char>(type);
    packet += static_cast<char>(seq & 0xFF);
    packet += static_cast<char>(seq >> 8);
    for (uint8_t byte : nonce) packet += static_cast<char>(byte);
    for (uint8_t i = 0; i < sizeof(sourceId_); ++i)
      packet += static_cast<char>((sourceId_ >> (8 * i)) & 0xFF);
    packet += static_cast<char>(ttl);

    uint8_t cipher[Config::LORA_MAX_PACKET] = {};
    uint8_t tag[PACKET_TAG] = {};
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const bool ok =
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
        mbedtls_gcm_crypt_and_tag(
            &gcm, MBEDTLS_GCM_ENCRYPT, len,
            nonce, sizeof(nonce),
            reinterpret_cast<const unsigned char*>(packet.c_str()),
            PACKET_HEADER_V4, plain, cipher, PACKET_TAG, tag) == 0;
    if (ok) {
      for (size_t i = 0; i < len; ++i) packet += static_cast<char>(cipher[i]);
      for (uint8_t byte : tag) packet += static_cast<char>(byte);
    }
    mbedtls_gcm_free(&gcm);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(nonce, sizeof(nonce));
    mbedtls_platform_zeroize(cipher, sizeof(cipher));
    mbedtls_platform_zeroize(tag, sizeof(tag));
    return ok;
  }

  if (len + PACKET_HEADER_V2 + PACKET_TAG > Config::LORA_MAX_PACKET)
    return false;

  const uint32_t nonce = esp_random();
  const uint8_t ttl = Config::LORA_INITIAL_TTL;
  packet.reserve(PACKET_HEADER_V2 + len + PACKET_TAG);
  packet += static_cast<char>(PACKET_MAGIC);
  packet += static_cast<char>(Config::LORA_PROTOCOL_VERSION);
  packet += static_cast<char>(type);
  packet += static_cast<char>(seq & 0xFF);
  packet += static_cast<char>(seq >> 8);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((nonce >> (8 * i)) & 0xFF);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((sourceId_ >> (8 * i)) & 0xFF);
  packet += static_cast<char>(ttl);

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &seq, sizeof(seq));
  uint8_t streamBlock[16] = {};
  uint8_t cipher[Config::LORA_MAX_PACKET] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
            mbedtls_aes_crypt_ctr(&aes, len, &ncOff, iv, streamBlock,
                                  plain, cipher) == 0;
  if (ok) {
    for (size_t i = 0; i < len; ++i)
      packet += static_cast<char>(cipher[i]);
    unsigned char tag[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    ok = md && mbedtls_md_hmac(md, key, sizeof(key),
                               reinterpret_cast<const unsigned char*>(packet.c_str()),
                               PACKET_HEADER_V2 + len, tag, sizeof(tag)) == 0;
    if (ok)
      for (size_t i = 0; i < PACKET_TAG; ++i)
        packet += static_cast<char>(tag[i]);
  }
  mbedtls_aes_free(&aes);
  return ok;
}

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
bool LoRaManager::encryptPacketV5(const uint8_t* plain, size_t len, uint8_t type,
                                  uint16_t seq, uint8_t hopIndex,
                                  uint32_t epochSec, uint32_t peerSourceId,
                                  uint8_t keyEpochDelta, String& packet) {
  if (!plain || peerSourceId == 0 || epochSec == 0 ||
      (keyEpochDelta != Config::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT &&
       keyEpochDelta != Config::LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS) ||
      len + PACKET_HEADER_V5 + PACKET_TAG > Config::LORA_MAX_PACKET)
    return false;

  uint32_t keyEpochSec = 0;
  if (!LoRaEcdhRekey::sessionEpochForDelta(
          epochSec, keyEpochDelta, keyEpochSec))
    return false;

  uint8_t sessionKey[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  bool haveKey = ecdhKeyMaterial_.getSessionKey(
      peerSourceId, keyEpochSec, sessionKey);

  if (!haveKey) {
    for (const auto& peer : ecdhPeers_) {
      if (!peer.valid || peer.sourceId != peerSourceId)
        continue;
      const uint32_t peerEpoch = LoRaEcdhRekey::epochNumber(peer.epochSec);
      if (peerEpoch != LoRaEcdhRekey::epochNumber(keyEpochSec))
        continue;
      if (ecdhKeyMaterial_.hasEphemeralKey() &&
          ecdhKeyMaterial_.ephemeralEpoch() == peerEpoch) {
        haveKey = ecdhKeyMaterial_.deriveSessionKey(
            peer.ephemeralPublic, sourceId_, peerSourceId, peer.epochSec) &&
                  ecdhKeyMaterial_.getSessionKey(
                      peerSourceId, keyEpochSec, sessionKey);
      }
      break;
    }
  }

  if (!haveKey) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "ECDH session key unavailable";
    mbedtls_platform_zeroize(sessionKey, sizeof(sessionKey));
    return false;
  }

  const uint32_t nonce = esp_random();
  const uint8_t ttl = Config::LORA_INITIAL_TTL;
  packet.reserve(PACKET_HEADER_V5 + len + PACKET_TAG);
  packet += static_cast<char>(PACKET_MAGIC);
  packet += static_cast<char>(Config::LORA_PROTOCOL_VERSION_ECDH);
  packet += static_cast<char>(type);
  packet += static_cast<char>(seq & 0xFF);
  packet += static_cast<char>(seq >> 8);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((nonce >> (8 * i)) & 0xFF);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((sourceId_ >> (8 * i)) & 0xFF);
  packet += static_cast<char>(ttl);
  packet += static_cast<char>(hopIndex);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((epochSec >> (8 * i)) & 0xFF);
  packet += static_cast<char>(keyEpochDelta);

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &seq, sizeof(seq));
  iv[6] = hopIndex;
  iv[7] = keyEpochDelta;
  memcpy(iv + 8, &epochSec, sizeof(epochSec));

  uint8_t streamBlock[16] = {};
  uint8_t cipher[Config::LORA_MAX_PACKET] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(
                &aes, sessionKey, LoRaEcdhRekey::AES_KEY_BYTES * 8U) == 0 &&
            mbedtls_aes_crypt_ctr(&aes, len, &ncOff, iv, streamBlock,
                                  plain, cipher) == 0;
  if (ok) {
    for (size_t i = 0; i < len; ++i)
      packet += static_cast<char>(cipher[i]);
    uint8_t tag[32] = {};
    const mbedtls_md_info_t* md =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    ok = md && mbedtls_md_hmac(
        md, sessionKey + LoRaEcdhRekey::AES_KEY_BYTES,
        LoRaEcdhRekey::HMAC_KEY_BYTES,
        reinterpret_cast<const unsigned char*>(packet.c_str()),
        PACKET_HEADER_V5 + len, tag, sizeof(tag)) == 0;
    if (ok)
      for (size_t i = 0; i < PACKET_TAG; ++i)
        packet += static_cast<char>(tag[i]);
    mbedtls_platform_zeroize(tag, sizeof(tag));
  }
  mbedtls_aes_free(&aes);
  mbedtls_platform_zeroize(sessionKey, sizeof(sessionKey));
  mbedtls_platform_zeroize(iv, sizeof(iv));
  mbedtls_platform_zeroize(streamBlock, sizeof(streamBlock));
  mbedtls_platform_zeroize(cipher, sizeof(cipher));
  return ok;
}
#endif

bool LoRaManager::encryptRoutedPacket(const uint8_t* plain, size_t len, uint8_t type,
                                       uint16_t seq, uint32_t destination,
                                       uint32_t excludeNextHop, String& packet) {
  if (!plain || len + ROUTE_EXT_BYTES > Config::LORA_MAX_PACKET -
      PACKET_HEADER_V2 - PACKET_TAG) return false;
  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const size_t routedLen = addRouteExtension(
      plain, len, destination, excludeNextHop, routed, sizeof(routed));
  if (!routedLen) return false;
  return encryptPacket(routed, routedLen, type, seq, packet);
}

bool LoRaManager::encryptPacketV3(const uint8_t* plain, size_t len, uint8_t type,
                                   uint16_t seq, uint8_t hopIndex, uint32_t epochSec,
                                   String& packet) {
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  if (!LoRaEcdhRekey::ecdhBeaconWireAllowed(3U, type)) {
    uint32_t peerSourceId = 0;
    if (plain && len >= 12 && plain[0] == ROUTE_EXT_MAGIC &&
        (plain[1] == ROUTE_EXT_VERSION ||
         plain[1] == ROUTE_EXT_VERSION_V1)) {
      uint32_t destination = 0;
      uint32_t nextHop = 0;
      memcpy(&destination, plain + 4, sizeof(destination));
      memcpy(&nextHop, plain + 8, sizeof(nextHop));
      peerSourceId = nextHop != 0 ? nextHop : destination;
    }
    if (peerSourceId == 0) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "ECDH peer unsupported";
      return false;
    }
    const uint32_t effectiveEpoch =
        epochSec != 0 ? epochSec : ecdhAuthoritativeEpochSec_;
    if (effectiveEpoch == 0)
      return false;
    return encryptPacketV5(
        plain, len, type, seq, hopIndex, effectiveEpoch, peerSourceId,
        Config::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT, packet);
  }
#endif
  uint8_t key[16];
  if (!plain || !loadKey(key) ||
      len + PACKET_HEADER_V3 + PACKET_TAG > Config::LORA_MAX_PACKET)
    return false;
  uint8_t rotatingKey[16] = {};
  if (epochSec != 0 && deriveRotatingKey(key, epochSec, rotatingKey)) memcpy(key, rotatingKey, sizeof(key));

  const uint32_t nonce = esp_random();
  packet.reserve(PACKET_HEADER_V3 + len + PACKET_TAG);
  packet += static_cast<char>(PACKET_MAGIC);
  packet += static_cast<char>(LORA_PROTOCOL_VERSION_HOP);
  packet += static_cast<char>(type);
  packet += static_cast<char>(seq & 0xFF);
  packet += static_cast<char>(seq >> 8);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((nonce >> (8 * i)) & 0xFF);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((sourceId_ >> (8 * i)) & 0xFF);
  packet += static_cast<char>(Config::LORA_INITIAL_TTL);
  packet += static_cast<char>(hopIndex);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((epochSec >> (8 * i)) & 0xFF);

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &seq, sizeof(seq));
  iv[6] = hopIndex;
  memcpy(iv + 8, &epochSec, sizeof(epochSec));
  uint8_t streamBlock[16] = {};
  uint8_t cipher[Config::LORA_MAX_PACKET] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
            mbedtls_aes_crypt_ctr(&aes, len, &ncOff, iv, streamBlock,
                                  plain, cipher) == 0;
  if (ok) {
    for (size_t i = 0; i < len; ++i)
      packet += static_cast<char>(cipher[i]);
    unsigned char tag[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    ok = md && mbedtls_md_hmac(md, key, sizeof(key),
        reinterpret_cast<const unsigned char*>(packet.c_str()),
        PACKET_HEADER_V3 + len, tag, sizeof(tag)) == 0;
    if (ok) for (size_t i = 0; i < PACKET_TAG; ++i)
      packet += static_cast<char>(tag[i]);
  }
  mbedtls_aes_free(&aes);
  return ok;
}

bool LoRaManager::decryptPacketV3(const String& packet, uint8_t& type,
                                  uint16_t& seq, uint32_t& sourceId,
                                  uint8_t& ttl, uint8_t& hopIndex,
                                  uint32_t& epochSec, uint8_t* plain,
                                  size_t capacity, size_t& len) {
  len = 0; sourceId = 0; ttl = 0; hopIndex = 0; epochSec = 0;
  EncryptedFrameParser::Parsed parsed{};
  if (!EncryptedFrameParser::parse(
          reinterpret_cast<const uint8_t*>(packet.c_str()), packet.length(),
          LORA_PROTOCOL_VERSION_HOP, PACKET_HEADER_V3, PACKET_TAG, parsed,
          Config::LORA_MAX_PACKET))
    return false;

  type = parsed.type;
  seq = parsed.sequence;
  uint32_t nonce = 0;
  memcpy(&nonce, packet.c_str() + 5, sizeof(nonce));
  sourceId = parsed.sourceId;
  ttl = parsed.ttl;
  hopIndex = parsed.hopIndex;
  epochSec = parsed.epochSec;

  const size_t cipherLen = parsed.cipherLength;
  if (!plain || cipherLen > capacity) return false;

  uint8_t key[16];
  if (!loadKey(key)) return false;
  uint8_t rotatingKey[16] = {};
  if (epochSec != 0 && deriveRotatingKey(key, epochSec, rotatingKey)) memcpy(key, rotatingKey, sizeof(key));
  unsigned char expected[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
      reinterpret_cast<const unsigned char*>(packet.c_str()),
      PACKET_HEADER_V3 + cipherLen, expected, sizeof(expected)) != 0)
    return false;

  const uint8_t* got = reinterpret_cast<const uint8_t*>(packet.c_str()) +
                       PACKET_HEADER_V3 + cipherLen;
  uint8_t diff = 0;
  for (size_t i = 0; i < PACKET_TAG; ++i) diff |= expected[i] ^ got[i];
  if (diff != 0) return false;

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &seq, sizeof(seq));
  iv[6] = hopIndex;
  memcpy(iv + 8, &epochSec, sizeof(epochSec));
  uint8_t streamBlock[16] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
      mbedtls_aes_crypt_ctr(&aes, cipherLen, &ncOff, iv, streamBlock,
          reinterpret_cast<const unsigned char*>(packet.c_str()) +
              PACKET_HEADER_V3, plain) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) return false;
  len = cipherLen;
  return true;
}

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
bool LoRaManager::decryptPacketV5(const String& packet, uint8_t& type,
                                  uint16_t& seq, uint32_t& sourceId,
                                  uint8_t& ttl, uint8_t& hopIndex,
                                  uint32_t& epochSec, uint8_t& keyEpochDelta,
                                  uint8_t* plain, size_t capacity,
                                  size_t& len) {
  len = 0;
  sourceId = 0;
  ttl = 0;
  hopIndex = 0;
  epochSec = 0;
  keyEpochDelta = 0;
  EncryptedFrameParser::Parsed parsed{};
  if (!EncryptedFrameParser::parse(
          reinterpret_cast<const uint8_t*>(packet.c_str()), packet.length(),
          Config::LORA_PROTOCOL_VERSION_ECDH, PACKET_HEADER_V5, PACKET_TAG,
          parsed, Config::LORA_MAX_PACKET))
    return false;

  type = parsed.type;
  seq = parsed.sequence;
  uint32_t nonce = 0;
  memcpy(&nonce, packet.c_str() + 5, sizeof(nonce));
  sourceId = parsed.sourceId;
  ttl = parsed.ttl;
  hopIndex = parsed.hopIndex;
  epochSec = parsed.epochSec;
  keyEpochDelta = parsed.keyEpochDelta;

  if (sourceId == 0 || ttl == 0 || ttl > Config::LORA_INITIAL_TTL ||
      epochSec == 0 ||
      (keyEpochDelta != Config::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT &&
       keyEpochDelta != Config::LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS))
    return false;

  const uint32_t localEpochSec =
      currentEpochSec() != 0 ? currentEpochSec() : ecdhAuthoritativeEpochSec_;
  if (localEpochSec == 0 ||
      !LoRaEcdhRekey::epochWithinSkew(localEpochSec, epochSec))
    return false;

  uint32_t keyEpochSec = 0;
  if (!LoRaEcdhRekey::sessionEpochForDelta(
          epochSec, keyEpochDelta, keyEpochSec))
    return false;

  const size_t cipherLen = parsed.cipherLength;
  if (!plain || cipherLen > capacity) return false;

  uint8_t sessionKey[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  if (!ecdhKeyMaterial_.getSessionKey(sourceId, keyEpochSec, sessionKey)) {
    mbedtls_platform_zeroize(sessionKey, sizeof(sessionKey));
    return false;
  }

  uint8_t expected[32] = {};
  const mbedtls_md_info_t* md =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const bool hmacOk = md && mbedtls_md_hmac(
      md, sessionKey + LoRaEcdhRekey::AES_KEY_BYTES,
      LoRaEcdhRekey::HMAC_KEY_BYTES,
      reinterpret_cast<const unsigned char*>(packet.c_str()),
      PACKET_HEADER_V5 + cipherLen, expected, sizeof(expected)) == 0;
  if (!hmacOk) {
    mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(sessionKey, sizeof(sessionKey));
    return false;
  }

  const uint8_t* got =
      reinterpret_cast<const uint8_t*>(packet.c_str()) +
      PACKET_HEADER_V5 + cipherLen;
  uint8_t diff = 0;
  for (size_t i = 0; i < PACKET_TAG; ++i)
    diff |= expected[i] ^ got[i];
  mbedtls_platform_zeroize(expected, sizeof(expected));
  if (diff != 0) {
    mbedtls_platform_zeroize(sessionKey, sizeof(sessionKey));
    return false;
  }

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &seq, sizeof(seq));
  iv[6] = hopIndex;
  iv[7] = keyEpochDelta;
  memcpy(iv + 8, &epochSec, sizeof(epochSec));
  uint8_t streamBlock[16] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  const bool ok =
      mbedtls_aes_setkey_enc(
          &aes, sessionKey, LoRaEcdhRekey::AES_KEY_BYTES * 8U) == 0 &&
      mbedtls_aes_crypt_ctr(
          &aes, cipherLen, &ncOff, iv, streamBlock,
          reinterpret_cast<const unsigned char*>(packet.c_str()) +
              PACKET_HEADER_V5,
          plain) == 0;
  mbedtls_aes_free(&aes);
  mbedtls_platform_zeroize(sessionKey, sizeof(sessionKey));
  mbedtls_platform_zeroize(iv, sizeof(iv));
  mbedtls_platform_zeroize(streamBlock, sizeof(streamBlock));
  if (!ok) return false;
  len = cipherLen;
  return true;
}
#endif

uint8_t LoRaManager::computeHopIndex(uint32_t frame) const {
  StateLock lock(gState);
  if (!lock.ok() || gState.hopChannelCount == 0) return 0;
  const uint32_t epochSec = hopSyncGps_ ? currentEpochSec() : 0;
  if (hopSyncGps_ && epochSec != 0) {
    const uint32_t dwellSec = max<uint32_t>(1, Config::HOP_DWELL_MS / 1000U);
    return static_cast<uint8_t>((epochSec / dwellSec) % gState.hopChannelCount);
  }
  return static_cast<uint8_t>(frame % gState.hopChannelCount);
}

bool LoRaManager::retuneToHopChannelLocked(uint8_t index) {
  if (index >= Config::HOP_CHANNEL_MAX) return false;
  uint8_t count = 0;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    count = gState.hopChannelCount;
    if (count == 0 || index >= count) return false;
  }
  const float freq = Config::LORA_MIN_FREQ_MHZ +
                     static_cast<float>(index) * Config::HOP_CHANNEL_STEP_MHZ;
  if (freq > Config::LORA_MAX_FREQ_MHZ) return false;
  SpiLock spiLock(pdMS_TO_TICKS(1000));
  if (!spiLock.ok()) return false;
  return radio_.setFrequency(freq) == RADIOLIB_ERR_NONE &&
         radio_.startReceive() == RADIOLIB_ERR_NONE;
}

bool LoRaManager::retuneToChannel0Locked() {
  SpiLock spiLock(pdMS_TO_TICKS(1000));
  if (!spiLock.ok()) return false;
  return radio_.setFrequency(gConfig.loraFreqMHz) == RADIOLIB_ERR_NONE &&
         radio_.startReceive() == RADIOLIB_ERR_NONE;
}

bool LoRaManager::retuneToHopChannel(uint8_t index) {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  const bool ok = retuneToHopChannelLocked(index);
  xSemaphoreGive(mutex_);
  return ok;
}

bool LoRaManager::retuneToChannel0() {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  const bool ok = retuneToChannel0Locked();
  xSemaphoreGive(mutex_);
  return ok;
}

bool LoRaManager::transmitHopped(const String& text, uint8_t type, uint32_t destination) {
  if (!ready_ || !mutex_ || text.isEmpty()) return false;

  // Establish the global lock order before entering the text transaction.
  // No text-state lock is held while acquiring mutex_ or StateLock.
  bool hopOn = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    hopOn = gState.hopEnabled && gState.hopChannelCount > 0;
  }

  bool textStateHeld = false;
  uint32_t sourceIdSnapshot = 0;
  if (type == Config::LORA_TYPE_TEXT) {
    if (!textStateMutex_ ||
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(50)) != pdTRUE)
      return false;
    if (textAwaitingAck_) {
      xSemaphoreGive(textStateMutex_);
      return false;
    }
    // Snapshot the authenticated origin while holding the same mutex used by
    // updateSourceId(). Route construction below must use this exact identity.
    sourceIdSnapshot = sourceId_;
    textStateHeld = true;
  }

  if (text.length() + ROUTE_EXT_BYTES > Config::LORA_MAX_PACKET - PACKET_HEADER_V3 - PACKET_TAG) {
    if (textStateHeld) xSemaphoreGive(textStateMutex_);
    return false;
  }
  const uint8_t hopIndex = hopOn ? computeHopIndex(hopFrame_) : 0;
  const uint32_t epochSec = currentEpochSec();
  uint16_t seq = 0;
  if (!nextTxSequence(seq)) {
    if (textStateHeld) xSemaphoreGive(textStateMutex_);
    return false;
  }
  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const size_t routedLen = addRouteExtension(
      reinterpret_cast<const uint8_t*>(text.c_str()), text.length(),
      destination, 0, routed, sizeof(routed), sourceIdSnapshot);
  if (!routedLen) {
    if (textStateHeld) xSemaphoreGive(textStateMutex_);
    return false;
  }
  String packet;
  if (!encryptPacketV3(routed, routedLen, type, seq, hopIndex, epochSec, packet)) {
    if (textStateHeld) xSemaphoreGive(textStateMutex_);
    return false;
  }

  if (type == Config::LORA_TYPE_TEXT) {
    textPendingPacket_ = packet;
    textPendingSeq_ = seq;
    textRetryCount_ = 0;
    textSentMs_ = millis();
    textAwaitingAck_ = true;
    textAcked_ = false;
    xSemaphoreGive(textStateMutex_);
    textStateHeld = false;
  }

  // Keep the same mutex -> SPI lock ordering used by the other TX paths.
  // Retuning before taking mutex_ can deadlock with processPendingTx(), which
  // takes mutex_ first and then waits for the global SPI mutex.
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    if (type == Config::LORA_TYPE_TEXT && textStateMutex_ &&
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
      textAwaitingAck_ = false;
      textAcked_ = false;
      xSemaphoreGive(textStateMutex_);
    }
    return false;
  }
  if (hopOn && !retuneToHopChannelLocked(hopIndex)) {
    xSemaphoreGive(mutex_);
    if (type == Config::LORA_TYPE_TEXT && textStateMutex_ &&
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
      textAwaitingAck_ = false;
      textAcked_ = false;
      xSemaphoreGive(textStateMutex_);
    }
    return false;
  }

  int16_t st = RADIOLIB_ERR_UNKNOWN;
  bool txOk = false;
  for (uint8_t attempt = 0; attempt <= Config::LORA_LBT_MAX_RETRIES; ++attempt) {
    {
      SpiLock spiLock(pdMS_TO_TICKS(1000));
      if (!spiLock.ok()) break;
      const int16_t scanSt = Config::LORA_LBT_ENABLED ? radio_.scanChannel() : RADIOLIB_CHANNEL_FREE;
      if (Config::LORA_LBT_ENABLED && lbtChannelBusy(scanSt)) st = scanSt;
      else if (scanSt == RADIOLIB_CHANNEL_FREE) {
        const RadioLibTime_t airtimeUs = radio_.getTimeOnAir(packet.length());
        if (airtimeUs != 0 && airtimeUs <= UINT32_MAX && consumeDutyBudget(static_cast<uint32_t>(airtimeUs))) {
          st = radio_.transmit(packet);
          txOk = st == RADIOLIB_ERR_NONE;
          if (txOk) {
            updateAntennaHealthAfterTx();
          }
          (void)radio_.startReceive();
        } else st = RADIOLIB_ERR_UNKNOWN;
      } else st = scanSt;
    }
    if (txOk || attempt >= Config::LORA_LBT_MAX_RETRIES) break;
    const uint32_t span = Config::LORA_LBT_BACKOFF_MAX_MS - Config::LORA_LBT_BACKOFF_MIN_MS;
    vTaskDelay(pdMS_TO_TICKS(Config::LORA_LBT_BACKOFF_MIN_MS + (span ? (esp_random() % (span + 1U)) : 0U)));
  }
  (void)retuneToChannel0Locked();
  xSemaphoreGive(mutex_);
  if (!txOk) {
    if (type == Config::LORA_TYPE_TEXT && textStateMutex_ &&
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      textAwaitingAck_ = false;
      textAcked_ = false;
      xSemaphoreGive(textStateMutex_);
    }
    return false;
  }
  ++hopFrame_;
  uint32_t loggedSourceId = 0;
  if (type == Config::LORA_TYPE_TEXT && textStateMutex_ &&
      xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    loggedSourceId = sourceId_;
    xSemaphoreGive(textStateMutex_);
  } else {
    loggedSourceId = sourceId_;
  }
  logPacket(true, type, seq, loggedSourceId, 0, 0.0f, Config::LORA_INITIAL_TTL);
  return true;
}

bool LoRaManager::decryptPacket(const String& packet, uint8_t& type, uint16_t& seq,
                                 uint32_t& sourceId, uint8_t& ttl,
                                 uint8_t* plain, size_t capacity, size_t& len) {
  len = 0; sourceId = 0; ttl = 0;
  if (packet.length() < PACKET_HEADER_V1 + PACKET_TAG ||
      static_cast<uint8_t>(packet[0]) != PACKET_MAGIC) return false;

  const uint8_t version = static_cast<uint8_t>(packet[1]);
  type = static_cast<uint8_t>(packet[2]);
  seq = static_cast<uint16_t>(static_cast<uint8_t>(packet[3])) |
        (static_cast<uint16_t>(static_cast<uint8_t>(packet[4])) << 8);

  if (version == Config::LORA_PROTOCOL_VERSION_GCM && Config::LORA_USE_AES_GCM) {
    if (packet.length() < PACKET_HEADER_V4 + 16) return false;
    uint8_t nonce[12] = {};
    memcpy(nonce, packet.c_str() + 5, sizeof(nonce));
    memcpy(&sourceId, packet.c_str() + 17, sizeof(sourceId));
    ttl = static_cast<uint8_t>(packet[21]);
    const size_t cipherLen = packet.length() - PACKET_HEADER_V4 - 16;
    if (!plain || cipherLen > capacity) return false;
    uint8_t key[16] = {};
    if (!loadKey(key)) return false;
    const uint8_t* cipher = reinterpret_cast<const uint8_t*>(packet.c_str()) + PACKET_HEADER_V4;
    const uint8_t* tag = cipher + cipherLen;
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
      mbedtls_gcm_auth_decrypt(&gcm, cipherLen, nonce, sizeof(nonce),
        reinterpret_cast<const unsigned char*>(packet.c_str()), PACKET_HEADER_V4,
        tag, 16, cipher, plain) == 0;
    mbedtls_gcm_free(&gcm);
    if (!ok) return false;
    len = cipherLen;
    return true;
  }

  // Legacy CTR/HMAC decoding is retained only for internal V3 hopping paths.
  size_t headerLen = 0;
  if (version == Config::LORA_PROTOCOL_VERSION) {
    headerLen = PACKET_HEADER_V2;
    memcpy(&sourceId, packet.c_str() + PACKET_HEADER_V1, sizeof(sourceId));
    ttl = static_cast<uint8_t>(packet[PACKET_HEADER_V1 + sizeof(sourceId)]);
  } else if (version == Config::LORA_LEGACY_PROTOCOL_VERSION) {
    headerLen = PACKET_HEADER_V1;
  } else {
    return false;
  }
  const size_t cipherLen = packet.length() - headerLen - PACKET_TAG;
  if (!plain || cipherLen > capacity) return false;
  uint8_t key[16] = {};
  if (!loadKey(key)) return false;
  uint32_t nonce = 0; memcpy(&nonce, packet.c_str() + 5, sizeof(nonce));
  unsigned char expected[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
      reinterpret_cast<const unsigned char*>(packet.c_str()), headerLen + cipherLen,
      expected, sizeof(expected)) != 0) return false;
  const uint8_t* got = reinterpret_cast<const uint8_t*>(packet.c_str()) + headerLen + cipherLen;
  uint8_t diff = 0; for (size_t i = 0; i < PACKET_TAG; ++i) diff |= expected[i] ^ got[i];
  if (diff) return false;
  uint8_t iv[16] = {}; memcpy(iv, &nonce, 4); memcpy(iv + 4, &seq, 2);
  uint8_t streamBlock[16] = {}; size_t ncOff = 0;
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
      mbedtls_aes_crypt_ctr(&aes, cipherLen, &ncOff, iv, streamBlock,
        reinterpret_cast<const unsigned char*>(packet.c_str()) + headerLen, plain) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) return false;
  len = cipherLen; return true;
}


uint32_t LoRaManager::hashPayload(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261UL;
  if (!data) return 0;
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t LoRaManager::sourceIdFromCallsign(const String& callsign) {
  const uint32_t hash = hashPayload(
      reinterpret_cast<const uint8_t*>(callsign.c_str()), callsign.length());
  return hash ? hash : 1;
}

bool LoRaManager::seenDedup(uint32_t sourceId, uint16_t seq, uint8_t type, uint32_t payloadHash, uint32_t packetEpochSec) {
  if (acceptReplay(sourceId, seq, type, payloadHash, packetEpochSec)) return true;
  const uint32_t now = millis();
  for (size_t i = 0; i < DEDUP_CACHE_SIZE; ++i) {
    DedupEntry& entry = dedupCache_[i];
    if (entry.seenMs == 0) continue;
    if (now - entry.seenMs >= Config::LORA_DEDUP_TTL_MS) {
      entry.seenMs = 0;
      continue;
    }
    if (entry.sourceId == sourceId && entry.seq == seq &&
        entry.payloadHash == payloadHash) {
      entry.seenMs = now;
      ++dedupHits_;
      return true;
    }
  }

  ++dedupMisses_;
  DedupEntry& slot = dedupCache_[dedupNext_];
  if (slot.seenMs != 0) ++dedupEvictions_;
  slot.sourceId = sourceId;
  slot.seq = seq;
  slot.payloadHash = payloadHash;
  slot.seenMs = now;
  dedupNext_ = (dedupNext_ + 1) % DEDUP_CACHE_SIZE;
  return false;
}


void LoRaManager::logPacket(bool tx, uint8_t type, uint16_t seq, uint32_t sourceId,
                            int16_t rssi, float snr, uint8_t ttl) {
  StateLock lock(gState);
  if (!lock.ok()) return;
  LoraPacketLogEntry& e = gState.loraPacketLog[gState.loraPacketLogNext];
  e.timestamp = gState.gps.timeValid ? gState.gps.utcEpoch : millis();
  e.tx = tx; e.type = type; e.seq = seq; e.sourceId = sourceId;
  e.rssi = rssi; e.snr = snr; e.ttl = ttl;
  gState.loraPacketLogNext =
      (gState.loraPacketLogNext + 1) % Config::LORA_PACKET_LOG_SIZE;
  if (gState.loraPacketLogCount < Config::LORA_PACKET_LOG_SIZE)
    ++gState.loraPacketLogCount;
}

void LoRaManager::addMessageHistory(uint32_t sourceId, const char* text) {
  if (!text) return;
  {
    StateLock lock(gState);
    if (!lock.ok()) return;
    MessageHistoryEntry& e = gState.messageHistory[gState.messageHistoryNext];
    e.timestamp = gState.gps.timeValid ? gState.gps.utcEpoch : 0;
    e.sourceId = sourceId;
    e.text = text;
    e.read = false;
    if (gState.messageHistoryCount == Config::MESSAGE_HISTORY_SIZE) {
      if (!gState.messageHistory[gState.messageHistoryNext].read && gState.messageUnreadCount)
        --gState.messageUnreadCount;
    }
    ++gState.messageUnreadCount;
    gState.messageHistoryNext =
        (gState.messageHistoryNext + 1) % Config::MESSAGE_HISTORY_SIZE;
    if (gState.messageHistoryCount < Config::MESSAGE_HISTORY_SIZE)
      ++gState.messageHistoryCount;
    gState.lastMessage = text;
  }
  messageHistoryDirty_ = true;
}

bool LoRaManager::forwardRateAllowed(uint32_t sourceId, uint8_t type) {
  const uint32_t now = millis();
  const uint32_t interval = type == 1
      ? Config::LORA_FORWARD_VOICE_RATE_LIMIT_MS
      : Config::LORA_FORWARD_RATE_LIMIT_MS;
  for (size_t i = 0; i < Config::LORA_FORWARD_SOURCE_CACHE_SIZE; ++i) {
    if (forwardSourceRates_[i].sourceId == sourceId) {
      if (now - forwardSourceRates_[i].lastMs < interval) return false;
      forwardSourceRates_[i].lastMs = now;
      return true;
    }
  }
  ForwardSourceRate& slot = forwardSourceRates_[forwardSourceNext_];
  slot.sourceId = sourceId;
  slot.lastMs = now;
  forwardSourceNext_ =
      (forwardSourceNext_ + 1) % Config::LORA_FORWARD_SOURCE_CACHE_SIZE;
  return true;
}

bool LoRaManager::enqueueForward(uint8_t type, uint16_t seq, uint32_t sourceId,
                                  uint8_t ttl, const uint8_t* payload, size_t len,
                                  uint8_t wireVersion) {
  if (!forwardQueue_ || !payload || !len || ttl <= 1 ||
      (wireVersion != Config::LORA_PROTOCOL_VERSION &&
       wireVersion != LORA_PROTOCOL_VERSION_HOP &&
       wireVersion != Config::LORA_PROTOCOL_VERSION_ECDH) ||
      len > Config::LORA_MAX_PACKET - PACKET_HEADER_TX - PACKET_TAG)
    return false;
  if (!forwardRateAllowed(sourceId, type)) return false;

  uint32_t destination = 0;
  uint32_t nextHop = 0;
  uint32_t previousHop = 0;
  uint8_t hopCount = 0;
  size_t routeOffset = 0;
  const bool hasRoute = parseRouteExtension(payload, len, destination, nextHop,
                                            previousHop, hopCount, routeOffset);
  if (hasRoute) {
    if (!routeAllowsForward(destination, nextHop, previousHop) ||
        nextHop == sourceId ||
        hopCount >= ROUTE_EXT_MAX_HOPS) return false;
  }

  // Queue the authenticated payload exactly as received. Route mutation must
  // happen once, immediately before transmission, when the next hop is known.
  // Rewriting it here and again in transmitForward() makes previousHop become
  // this node and causes routeAllowsForward() to reject our own queued packet.
  if (len > Config::LORA_MAX_PACKET - PACKET_HEADER_TX - PACKET_TAG)
    return false;

  ForwardPacket packet{};
  packet.type = type;
  packet.wireVersion = wireVersion;
  packet.ttl = static_cast<uint8_t>(ttl - 1);
  packet.seq = seq;
  packet.sourceId = sourceId;
  packet.dedupId = hashPayload(payload, len) ^ sourceId ^
                   (static_cast<uint32_t>(seq) << 16);
  packet.receivedMs = millis();
  packet.priority = type == Config::LORA_TYPE_SOS ? TX_PRIORITY_SOS :
                    type == Config::LORA_TYPE_VOICE ? TX_PRIORITY_VOICE : TX_PRIORITY_FORWARD;
  packet.persistId = packet.dedupId;
  packet.len = static_cast<uint16_t>(len);
  memcpy(packet.payload, payload, len);
  bool queued = false;
  for (uint8_t attempt = 0; attempt < 3 && !queued; ++attempt) {
    queued = xQueueSend(forwardQueue_, &packet,
                        attempt == 0 ? 0 : pdMS_TO_TICKS(2)) == pdPASS;
    if (!queued) taskYIELD();
  }
  if (!queued) {
    ++forwardDrops_;
    forwardLastDropMs_ = millis();
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "LoRa forward queue full; packet dropped";
    return false;
  }
  (void)persistForwardQueue();
  return true;
}

bool LoRaManager::persistForwardQueue() {
  if (storageResetting_.load(std::memory_order_acquire) || !storage.ready() || !forwardQueue_) return false;
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;
  if (!SD.exists("/LORA") && !SD.mkdir("/LORA")) return false;

  const char* tmpPath = "/LORA/FWD.NEW";
  if (SD.exists(tmpPath)) SD.remove(tmpPath);
  File f = SD.open(tmpPath, FILE_WRITE);
  if (!f) return false;

  ForwardPacket items[FORWARD_QUEUE_DEPTH] = {};
  UBaseType_t count = 0;
  while (count < FORWARD_QUEUE_DEPTH && xQueueReceive(forwardQueue_, &items[count], 0) == pdPASS)
    ++count;

  auto restoreQueue = [&]() {
    for (UBaseType_t i = 0; i < count; ++i)
      (void)xQueueSend(forwardQueue_, &items[i], 0);
  };

  uint8_t key[16] = {};
  if (!loadKey(key)) {
    f.close();
    restoreQueue();
    return false;
  }
  for (UBaseType_t i = 0; i < count; ++i) {
    const ForwardPacket& item = items[i];
    if (!item.len || item.len > Config::LORA_MAX_PACKET - PACKET_HEADER_TX - PACKET_TAG) continue;
    const uint32_t nonce = esp_random();
    uint8_t iv[16] = {};
    memcpy(iv, &nonce, sizeof(nonce));
    memcpy(iv + 4, &item.seq, sizeof(item.seq));
    memcpy(iv + 8, &item.sourceId, sizeof(item.sourceId));
    uint8_t cipher[Config::LORA_MAX_PACKET] = {};
    uint8_t streamBlock[16] = {};
    size_t ncOff = 0;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
              mbedtls_aes_crypt_ctr(&aes, item.len, &ncOff, iv, streamBlock,
                                    item.payload, cipher) == 0;
    mbedtls_aes_free(&aes);
    if (!ok) {
      f.close();
      restoreQueue();
      return false;
    }

    uint8_t header[FORWARD_RECORD_FIXED] = {};
    size_t o = 0;
    header[o++] = static_cast<uint8_t>(FORWARD_RECORD_MAGIC & 0xFF);
    header[o++] = static_cast<uint8_t>(FORWARD_RECORD_MAGIC >> 8);
    header[o++] = FORWARD_RECORD_VERSION;
    header[o++] = item.type;
    header[o++] = item.ttl;
    header[o++] = item.priority;
    memcpy(header + o, &item.seq, 2); o += 2;
    memcpy(header + o, &item.sourceId, 4); o += 4;
    memcpy(header + o, &item.len, 2); o += 2;
    memcpy(header + o, &nonce, 4); o += 4;
    header[o++] = item.wireVersion;
    if (f.write(header, sizeof(header)) != sizeof(header) ||
        f.write(cipher, item.len) != item.len) {
      f.close();
      restoreQueue();
      return false;
    }

    uint8_t tag[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    // Bind ciphertext to the authenticated record without changing packet v2.
    uint8_t bind[Config::LORA_MAX_PACKET + FORWARD_RECORD_FIXED] = {};
    memcpy(bind, header, sizeof(header));
    memcpy(bind + sizeof(header), cipher, item.len);
    if (mbedtls_md_hmac(md, key, sizeof(key), bind, sizeof(header) + item.len, tag, sizeof(tag)) != 0) {
      f.close();
      restoreQueue();
      return false;
    }
    if (f.write(tag, PACKET_TAG) != PACKET_TAG) {
      f.close();
      restoreQueue();
      return false;
    }
  }
  f.flush();
  f.close();

  // Never delete the last known-good queue before the replacement is safely
  // renamed. Keep a backup so a reset between rename steps can be recovered.
  if (SD.exists(FORWARD_QUEUE_BACKUP_FILE)) SD.remove(FORWARD_QUEUE_BACKUP_FILE);
  if (SD.exists(FORWARD_QUEUE_FILE) &&
      !SD.rename(FORWARD_QUEUE_FILE, FORWARD_QUEUE_BACKUP_FILE)) {
    if (SD.exists(tmpPath)) SD.remove(tmpPath);
    restoreQueue();
    return false;
  }
  if (!SD.rename(tmpPath, FORWARD_QUEUE_FILE)) {
    if (SD.exists(FORWARD_QUEUE_BACKUP_FILE))
      (void)SD.rename(FORWARD_QUEUE_BACKUP_FILE, FORWARD_QUEUE_FILE);
    if (SD.exists(tmpPath)) SD.remove(tmpPath);
    restoreQueue();
    return false;
  }
  if (SD.exists(FORWARD_QUEUE_BACKUP_FILE)) SD.remove(FORWARD_QUEUE_BACKUP_FILE);
  restoreQueue();
  return true;
}

bool LoRaManager::loadForwardQueue() {
  if (!storage.ready() || !forwardQueue_) return true;
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;

  const char* candidates[] = {FORWARD_QUEUE_FILE, FORWARD_QUEUE_BACKUP_FILE};
  uint8_t key[16] = {};
  if (!loadKey(key)) return false;

  auto drainQueue = [&]() {
    if (!forwardQueue_) return;
    ForwardPacket discarded{};
    while (xQueueReceive(forwardQueue_, &discarded, 0) == pdPASS) {}
  };

  auto tryLoad = [&](const char* path) -> bool {
    if (!SD.exists(path)) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    ForwardPacket decoded[Config::LORA_STORE_FORWARD_MAX_RECORDS] = {};
    size_t decodedCount = 0;
    uint8_t header[FORWARD_RECORD_FIXED] = {};
    bool validFile = true;

    while (f.available()) {
      if (decodedCount >= Config::LORA_STORE_FORWARD_MAX_RECORDS ||
          f.available() < static_cast<int>(FORWARD_RECORD_FIXED_V1)) {
        validFile = false;
        break;
      }
      memset(header, 0, sizeof(header));
      if (f.read(header, FORWARD_RECORD_FIXED_V1) != FORWARD_RECORD_FIXED_V1) {
        validFile = false;
        break;
      }

      size_t o = 0;
      const uint16_t magic = static_cast<uint16_t>(header[o]) |
                             (static_cast<uint16_t>(header[o + 1]) << 8); o += 2;
      const uint8_t version = header[o++];
      const uint8_t type = header[o++];
      const uint8_t ttl = header[o++];
      const uint8_t priority = header[o++];
      uint16_t seq = 0; memcpy(&seq, header + o, 2); o += 2;
      uint32_t sourceId = 0; memcpy(&sourceId, header + o, 4); o += 4;
      uint16_t len = 0; memcpy(&len, header + o, 2); o += 2;
      uint32_t nonce = 0; memcpy(&nonce, header + o, 4); o += 4;

      uint8_t wireVersion = Config::LORA_PROTOCOL_VERSION;
      size_t headerLen = FORWARD_RECORD_FIXED_V1;
      if (version == FORWARD_RECORD_VERSION) {
        if (f.read(&wireVersion, 1) != 1) { validFile = false; break; }
        header[FORWARD_RECORD_FIXED_V1] = wireVersion;
        headerLen = FORWARD_RECORD_FIXED;
      } else if (version != 1) {
        validFile = false;
        break;
      }

      if (magic != FORWARD_RECORD_MAGIC || ttl == 0 || len == 0 ||
          len > Config::LORA_MAX_PACKET - PACKET_HEADER_TX - PACKET_TAG ||
          (wireVersion != Config::LORA_PROTOCOL_VERSION &&
           wireVersion != LORA_PROTOCOL_VERSION_HOP &&
           wireVersion != Config::LORA_PROTOCOL_VERSION_ECDH)) {
        validFile = false;
        break;
      }

      uint8_t cipher[Config::LORA_MAX_PACKET] = {};
      uint8_t tag[PACKET_TAG] = {};
      if (f.read(cipher, len) != len || f.read(tag, PACKET_TAG) != PACKET_TAG) {
        validFile = false;
        break;
      }

      uint8_t bind[Config::LORA_MAX_PACKET + FORWARD_RECORD_FIXED] = {};
      memcpy(bind, header, headerLen);
      memcpy(bind + headerLen, cipher, len);
      uint8_t expected[32] = {};
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      if (!md || mbedtls_md_hmac(md, key, sizeof(key), bind, headerLen + len,
                                 expected, sizeof(expected)) != 0) {
        validFile = false;
        break;
      }
      uint8_t diff = 0;
      for (size_t i = 0; i < PACKET_TAG; ++i) diff |= expected[i] ^ tag[i];
      if (diff != 0) {
        validFile = false;
        break;
      }

      uint8_t iv[16] = {};
      memcpy(iv, &nonce, 4);
      memcpy(iv + 4, &seq, 2);
      memcpy(iv + 8, &sourceId, 4);
      uint8_t plain[Config::LORA_MAX_PACKET] = {};
      uint8_t streamBlock[16] = {};
      size_t ncOff = 0;
      mbedtls_aes_context aes;
      mbedtls_aes_init(&aes);
      const bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
                      mbedtls_aes_crypt_ctr(&aes, len, &ncOff, iv, streamBlock,
                                            cipher, plain) == 0;
      mbedtls_aes_free(&aes);
      if (!ok) {
        validFile = false;
        break;
      }

      ForwardPacket& item = decoded[decodedCount++];
      item.type = type;
      item.wireVersion = wireVersion;
      item.ttl = ttl;
      item.priority = priority;
      item.seq = seq;
      item.sourceId = sourceId;
      item.len = len;
      item.receivedMs = millis();
      item.dedupId = hashPayload(plain, len) ^ sourceId ^
                     (static_cast<uint32_t>(seq) << 16);
      item.persistId = item.dedupId;
      memcpy(item.payload, plain, len);
    }
    f.close();

    if (!validFile) return false;
    drainQueue();
    for (size_t i = 0; i < decodedCount; ++i)
      if (xQueueSend(forwardQueue_, &decoded[i], 0) != pdPASS) {
        drainQueue();
        return false;
      }
    return true;
  };

  // FWD.Q is accepted only after the complete file has been validated.
  // If it is torn/corrupt, discard the partial queue state and recover from
  // the last known-good .BAK instead.
  for (const char* path : candidates) {
    if (!SD.exists(path)) continue;
    if (tryLoad(path)) return true;
  }

  drainQueue();
  StateLock lock(gState);
  if (lock.ok()) gState.lastError = "No valid forward queue snapshot";
  return false;
}

bool LoRaManager::parseRouteExtension(const uint8_t* payload, size_t len,
                                       uint32_t& destination, uint32_t& nextHop,
                                       uint32_t& previousHop, uint8_t& hopCount,
                                       size_t& payloadOffset) const {
  destination = nextHop = previousHop = 0;
  hopCount = 0;
  payloadOffset = 0;
  if (!payload || len < ROUTE_EXT_V1_BYTES ||
      payload[0] != ROUTE_EXT_MAGIC)
    return false;

  const uint8_t version = payload[1];
  const size_t extBytes = version == ROUTE_EXT_VERSION ? ROUTE_EXT_V2_BYTES :
                          version == ROUTE_EXT_VERSION_V1 ? ROUTE_EXT_V1_BYTES : 0;
  if (!extBytes || len < extBytes) return false;

  const uint8_t flags = payload[2];
  hopCount = payload[3];
  memcpy(&destination, payload + 4, 4);
  memcpy(&nextHop, payload + 8, 4);
  if (hopCount > ROUTE_EXT_MAX_HOPS ||
      (flags & static_cast<uint8_t>(~ROUTE_EXT_FLAG_BROADCAST)) != 0 ||
      ((flags & ROUTE_EXT_FLAG_BROADCAST) != 0 && destination != 0))
    return false;

  if (version == ROUTE_EXT_VERSION_V1) {
    memcpy(&previousHop, payload + 12, 4);
  } else {
    // Rev-C route extension v2 carries the complete visited-node path:
    // path[0] is the origin, path[hopCount] is the current forwarding node.
    // Reject a frame if our own source ID already appears in the path.
    for (uint8_t i = 0; i <= hopCount; ++i) {
      uint32_t node = 0;
      memcpy(&node, payload + 12U + static_cast<size_t>(i) * sizeof(uint32_t), 4);
      if (node == 0) return false;
      if (node == sourceId_) return false;
      if (i > 0) {
        uint32_t prev = 0;
        memcpy(&prev, payload + 12U + static_cast<size_t>(i - 1U) * sizeof(uint32_t), 4);
        if (node == prev) return false;
      }
      previousHop = node;
    }
  }

  if (destination == sourceId_ || nextHop == sourceId_ || nextHop == 0) {
    payloadOffset = extBytes;
    return true;
  }
  payloadOffset = extBytes;
  return true;
}

uint16_t LoRaManager::calculateEtxQ8(uint16_t attempts, uint16_t success) const {
  if (!attempts || !success) return 256;
  return static_cast<uint16_t>(
      min<uint32_t>(ROUTE_ETX_MAX_Q8,
                    256UL * attempts / max<uint16_t>(1, success)));
}

uint32_t LoRaManager::selectNextHop(uint32_t destination, uint32_t excludeNextHop) const {
  const uint32_t now = millis();
  uint32_t best = 0;
  uint32_t bestScore = 0;

  // Prefer a fresh learned route to the requested destination. Its ETX is
  // combined with the current next-hop RSSI/SNR quality.
  if (destination != 0) {
    for (const auto& route : routes_) {
      if (route.destination != destination || route.nextHop == 0 ||
          route.nextHop == excludeNextHop || now - route.seenMs > ROUTE_CACHE_TTL_MS)
        continue;
      uint8_t q = route.quality;
      for (const auto& n : neighbors_) {
        if (n.sourceId == route.nextHop && now - n.seenMs <= Config::NEIGHBOR_TTL_MS) {
          q = n.quality;
          break;
        }
      }
      const uint32_t etxPenalty = min<uint32_t>(1000U, route.etxQ8 * 100U / 256U);
      const uint32_t score = static_cast<uint32_t>(q) * 7U +
                             (1000U - etxPenalty) * 3U / 10U;
      if (score > bestScore) {
        bestScore = score;
        best = route.nextHop;
      }
    }
    // Direct neighbor is always a valid destination route.
    for (const auto& n : neighbors_) {
      if (n.sourceId != destination || n.sourceId == excludeNextHop ||
          now - n.seenMs > Config::NEIGHBOR_TTL_MS) continue;
      const uint16_t etxQ8 = calculateEtxQ8(n.txAttempts, n.txSuccess);
      const uint32_t score = static_cast<uint32_t>(n.quality) * 7U +
                             (1000U - min<uint32_t>(1000U, etxQ8 * 100U / 256U)) * 3U / 10U;
      if (score >= bestScore) {
        bestScore = score;
        best = n.sourceId;
      }
    }
    if (best != 0) return best;
  }

  // Broadcast/unknown destination: selective forwarding chooses the strongest
  // fresh neighbor, while explicitly avoiding the previous hop.
  for (const auto& n : neighbors_) {
    if (n.sourceId == 0 || n.sourceId == excludeNextHop ||
        now - n.seenMs > Config::NEIGHBOR_TTL_MS) continue;
    const uint16_t etxQ8 = calculateEtxQ8(n.txAttempts, n.txSuccess);
    const uint32_t score = static_cast<uint32_t>(n.quality) * 7U +
                           (1000U - min<uint32_t>(1000U, etxQ8 * 100U / 256U)) * 3U / 10U;
    if (score > bestScore) {
      bestScore = score;
      best = n.sourceId;
    }
  }
  return best;
}

size_t LoRaManager::addRouteExtension(const uint8_t* payload, size_t len,
                                       uint32_t destination, uint32_t excludeNextHop,
                                       uint8_t* out, size_t capacity,
                                       uint32_t sourceIdOverride) const {
  const uint32_t origin = sourceIdOverride != 0 ? sourceIdOverride : sourceId_;
  if (!payload || !out || destination == origin ||
      len + ROUTE_EXT_V2_BYTES > capacity) return 0;
  const uint32_t nextHop = selectNextHop(destination, excludeNextHop);
  if (destination != 0 && nextHop == 0) return 0;
  const uint8_t flags = destination == 0 ? ROUTE_EXT_FLAG_BROADCAST : 0;
  const uint8_t hopCount = 0;
  out[0] = ROUTE_EXT_MAGIC;
  out[1] = ROUTE_EXT_VERSION;
  out[2] = flags;
  out[3] = hopCount;
  memcpy(out + 4, &destination, 4);
  memcpy(out + 8, &nextHop, 4);
  memset(out + 12, 0, ROUTE_EXT_V2_BYTES - 12);
  memcpy(out + 12, &origin, 4);
  memcpy(out + ROUTE_EXT_V2_BYTES, payload, len);
  return len + ROUTE_EXT_V2_BYTES;
}

bool LoRaManager::routeAllowsForward(uint32_t destination, uint32_t nextHop,
                                     uint32_t previousHop) const {
  if (destination == sourceId_) return false;
  if (nextHop != 0 && nextHop != sourceId_) return false;
  if (previousHop == sourceId_) return false;
  return true;
}

void LoRaManager::learnRoute(uint32_t destination, uint32_t nextHop,
                             int16_t rssi, float snr) {
  if (destination == 0 || destination == sourceId_ || nextHop == 0 ||
      nextHop == sourceId_) return;
  const uint32_t now = millis();
  size_t selected = ROUTE_CACHE_SIZE;
  for (size_t i = 0; i < ROUTE_CACHE_SIZE; ++i) {
    if (routes_[i].destination == destination) { selected = i; break; }
    if (selected == ROUTE_CACHE_SIZE && routes_[i].seenMs == 0) selected = i;
  }
  if (selected == ROUTE_CACHE_SIZE) {
    selected = routeNext_;
    routeNext_ = (routeNext_ + 1) % ROUTE_CACHE_SIZE;
  }
  auto& route = routes_[selected];
  route.destination = destination;
  route.nextHop = nextHop;
  route.quality = 0;
  for (const auto& n : neighbors_) {
    if (n.sourceId == nextHop && now - n.seenMs <= Config::NEIGHBOR_TTL_MS) {
      route.quality = n.quality;
      break;
    }
  }
  if (route.quality == 0) {
    route.quality = static_cast<uint8_t>(constrain(
        (rssi + 120) * 2 + static_cast<int>(snr * 3.0f) + 60, 0, 100));
  }
  route.etxQ8 = 256;
  for (const auto& n : neighbors_) {
    if (n.sourceId == nextHop) {
      route.etxQ8 = calculateEtxQ8(n.txAttempts, n.txSuccess);
      break;
    }
  }
  route.seenMs = now;
}

void LoRaManager::recordNeighborTxResult(uint32_t peerSourceId, bool success) {
  if (peerSourceId == 0 || peerSourceId == sourceId_) return;
  for (auto& n : neighbors_) {
    if (n.sourceId != peerSourceId) continue;
    if (n.txAttempts != UINT16_MAX) ++n.txAttempts;
    if (success && n.txSuccess != UINT16_MAX) ++n.txSuccess;
    for (auto& route : routes_) {
      if (route.nextHop == peerSourceId)
        route.etxQ8 = calculateEtxQ8(n.txAttempts, n.txSuccess);
    }
    return;
  }
}

bool LoRaManager::isPttOrRecording() const {
  StateLock lock(gState);
  return !lock.ok() || gState.ptt || gState.recording;
}

bool LoRaManager::transmitForward(const ForwardPacket& forward) {
  if (!ready_ || !mutex_ || !forward.len || forward.ttl == 0 ||
      forward.len > Config::LORA_MAX_PACKET - PACKET_HEADER_TX - PACKET_TAG ||
      isPttOrRecording())
    return false;

  String packet;

  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  size_t routedLen = 0;
  uint32_t destination = 0;
  uint32_t nextHop = 0;
  uint32_t txNextHop = 0;
  uint32_t previousHop = 0;
  uint8_t hopCount = 0;
  size_t routeOffset = 0;
  const bool hasRoute = parseRouteExtension(
      forward.payload, forward.len, destination, nextHop, previousHop,
      hopCount, routeOffset);
  if (hasRoute) {
    if (!routeAllowsForward(destination, nextHop, previousHop) ||
        hopCount >= ROUTE_EXT_MAX_HOPS) return false;
    uint32_t selectedNextHop = selectNextHop(destination, previousHop);
    if (forwardRetryPersistId_ == forward.persistId && forwardRetryPersistId_ != 0) {
      selectedNextHop = forwardInFlightNextHop_;
    }
    if (selectedNextHop == 0 || selectedNextHop == previousHop ||
        selectedNextHop == forward.sourceId) return false;
    txNextHop = selectedNextHop;
    const uint8_t routeVersion = forward.payload[1];
    const size_t outRouteBytes = routeVersion == ROUTE_EXT_VERSION
        ? ROUTE_EXT_V2_BYTES : ROUTE_EXT_V1_BYTES;
    const size_t appLen = forward.len - routeOffset;
    if (appLen + outRouteBytes > sizeof(routed)) return false;

    if (routeVersion == ROUTE_EXT_VERSION) {
      memcpy(routed, forward.payload, ROUTE_EXT_V2_BYTES);
      routed[3] = static_cast<uint8_t>(hopCount + 1U);
      memcpy(routed + 8, &selectedNextHop, 4);
      memcpy(routed + 12U + static_cast<size_t>(hopCount + 1U) * sizeof(uint32_t),
             &sourceId_, sizeof(sourceId_));
    } else {
      // Upgrade a legacy v1 extension to Rev-C v2 while preserving its known
      // origin/previous-hop information. Full history is only available from
      // packets emitted after the Rev-C upgrade.
      routed[0] = ROUTE_EXT_MAGIC;
      routed[1] = ROUTE_EXT_VERSION;
      routed[2] = destination == 0 ? ROUTE_EXT_FLAG_BROADCAST : 0;
      routed[3] = static_cast<uint8_t>(hopCount + 1U);
      memcpy(routed + 4, &destination, 4);
      memcpy(routed + 8, &selectedNextHop, 4);
      memset(routed + 12, 0, ROUTE_EXT_V2_BYTES - 12);
      uint32_t origin = previousHop;
      memcpy(routed + 12, &origin, 4);
      memcpy(routed + 16, &sourceId_, 4);
    }
    memcpy(routed + outRouteBytes, forward.payload + routeOffset, appLen);
    routedLen = appLen + outRouteBytes;
  } else {
    routedLen = addRouteExtension(forward.payload, forward.len, 0, 0,
                                  routed, sizeof(routed));
  }
  if (!routedLen) return false;

  // Preserve the hop-aware wire version. The old implementation silently
  // downgraded authenticated V3 frames to V2, discarding hop synchronization
  // metadata and (when rekeying is enabled) the V3 key epoch.
  const bool useV3 = forward.wireVersion == LORA_PROTOCOL_VERSION_HOP;
  const uint8_t txHopIndex = useV3 ? computeHopIndex(hopFrame_) : 0;
  const uint32_t txEpochSec = useV3 ? currentEpochSec() : 0;
  if (useV3) {
    if (!encryptPacketV3(routed, routedLen, forward.type, forward.seq,
                         txHopIndex, txEpochSec, packet))
      return false;
  } else {
    if (!encryptPacket(routed, routedLen, forward.type, forward.seq, packet))
      return false;
  }
  if (packet.length() > Config::LORA_MAX_PACKET) return false;

  // Keep forwarding serialized with normal TX. The pending TX state machine
  // performs CAD/backoff and preserves this already-built forwarding envelope.
  if (Config::LORA_LBT_ENABLED) {
    // Publish the original ForwardPacket before making the wire packet visible
    // to processPendingTx(). Otherwise the LoRa task can finish an immediate
    // CAD/TX attempt before forwardInFlight_ is initialized, losing retry state.
    if (forwardInFlightActive_) return false;
    forwardInFlight_ = forward;
    forwardInFlightNextHop_ = hasRoute ? selectedNextHop : 0;
    forwardRetryPersistId_ = forward.persistId;
    forwardInFlightActive_ = true;
    if (!queuePendingTx(packet, forward.priority)) {
      forwardInFlightActive_ = false;
      forwardInFlightNextHop_ = 0;
      forwardRetryPersistId_ = 0;
      return false;
    }
    // On an LBT retry, processPendingTx() requeues this original envelope,
    // never the already-mutated routed wire packet.
    (void)processPendingTx();
    return true;
  }

  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  if (useV3 && !retuneToHopChannelLocked(txHopIndex)) {
    xSemaphoreGive(mutex_);
    return false;
  }
  int16_t st = RADIOLIB_ERR_NONE;
  int16_t rxSt = RADIOLIB_ERR_NONE;
  bool budgetConsumed = false;
  RadioLibTime_t airtimeUs = 0;
  {
    SpiLock spiLock(pdMS_TO_TICKS(1000));
    if (spiLock.ok()) {
      airtimeUs = radio_.getTimeOnAir(packet.length());
      if (airtimeUs != 0 && airtimeUs <= UINT32_MAX &&
          consumeDutyBudget(static_cast<uint32_t>(airtimeUs))) {
        budgetConsumed = true;
        const uint32_t txStartMs = millis();
        st = radio_.transmit(packet);
        if (millis() - txStartMs > Config::LORA_TX_TIMEOUT_MS)
          st = RADIOLIB_ERR_TX_TIMEOUT;
        rxSt = radio_.startReceive();
        if (st != RADIOLIB_ERR_NONE)
          refundDutyBudget(static_cast<uint32_t>(airtimeUs));
      } else {
        st = -1;
      }
    } else {
      st = -1;
    }
  }

  bool txOk = budgetConsumed && st == RADIOLIB_ERR_NONE;
  if (txOk) updateAntennaHealthAfterTx();
  if (hasRoute) recordNeighborTxResult(txNextHop, txOk);
  {
    StateLock lock(gState);
    if (lock.ok()) {
      if (!txOk && st == -1)
        gState.lastError = "LoRa forward duty budget unavailable";
      if (rxSt != RADIOLIB_ERR_NONE) {
        ready_ = false;
        gState.loraReady = false;
        gState.lastError = "SX1262 RX restart failed after forward";
      }
      if (txOk) gState.txPackets++;
    }
  }
  if (txOk) logPacket(true, forward.type, forward.seq, forward.sourceId, 0, 0.0f, forward.ttl);
  if (txOk && useV3 && !retuneToChannel0Locked()) {
    // The frame is already on-air. Do not report failure and requeue it,
    // otherwise a successful TX would be retransmitted after a retune error.
    ready_ = false;
    StateLock lock(gState);
    if (lock.ok()) {
      gState.loraReady = false;
      gState.lastError = "SX1262 channel-0 retune failed after forward";
    }
  }
  xSemaphoreGive(mutex_);
  return txOk;
}


bool LoRaManager::scannerStart(uint8_t mode, uint16_t dwellMs) {
  if (!ready_ || !mutex_ || (mode != 1 && mode != 2) ||
      dwellMs < Config::SCANNER_MIN_DWELL_MS ||
      dwellMs > Config::SCANNER_MAX_DWELL_MS || isPttOrRecording())
    return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  scanner_.active = true;
  scanner_.mode = mode;
  scanner_.dwellMs = dwellMs;
  scanner_.index = 0;
  scanner_.sweepCount = 0;
  scanner_.lastSampleMs = 0;
  ++scannerGeneration_;
  for (auto& result : scanner_.results) result = ScannerState::Result{};
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.scannerActive = true;
      gState.scannerMode = mode;
      gState.scannerSweepCount = 0;
      gState.scannerSweepInProgress = true;
      gState.scannerChannelCount = Config::SCANNER_MAX_CHANNELS;
      gState.scannerDwellMs = dwellMs;
    }
  }
  xSemaphoreGive(mutex_);
  return true;
}

bool LoRaManager::scannerStop() {
  if (!mutex_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  scanner_.active = false;
  (void)retuneToChannel0Locked();
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.scannerActive = false;
      gState.scannerSweepInProgress = false;
    }
  }
  xSemaphoreGive(mutex_);
  return true;
}

bool LoRaManager::scannerIsActive() const {
  return scanner_.active;
}

void LoRaManager::scannerGetResults(ChannelScanResult* results, size_t& count) {
  count = 0;
  if (!results || !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE)
    return;
  const size_t n = min(static_cast<size_t>(Config::SCANNER_MAX_CHANNELS),
                       static_cast<size_t>(Config::HOP_CHANNEL_MAX));
  for (size_t i = 0; i < n; ++i) {
    if (scanner_.results[i].timestamp == 0 || scanner_.results[i].generation != scannerGeneration_) continue;
    results[count].freqMHz = scanner_.results[i].freqMHz;
    results[count].rssiAvgDbm = scanner_.results[i].rssiAvgDbm;
    results[count].rssiPeakDbm = scanner_.results[i].rssiPeakDbm;
    results[count].snrDb = scanner_.results[i].snrDb;
    results[count].occupancyPercent = scanner_.results[i].occupancyPercent;
    results[count].preambleCount = scanner_.results[i].preambleCount;
    results[count].timestamp = scanner_.results[i].timestamp;
    ++count;
  }
  xSemaphoreGive(mutex_);
}

size_t LoRaManager::scannerSuggestBestChannels(uint8_t* channels, size_t capacity) {
  if (!channels || capacity == 0 || !mutex_ ||
      xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE)
    return 0;
  if (!scanner_.active) {
    xSemaphoreGive(mutex_);
    return 0;
  }
  const size_t n = min(capacity, static_cast<size_t>(Config::HOP_CHANNEL_MAX));
  bool used[Config::HOP_CHANNEL_MAX] = {};
  size_t out = 0;
  while (out < n) {
    size_t best = Config::HOP_CHANNEL_MAX;
    uint8_t bestLoad = 255;
    int16_t bestRssi = 127;
    for (size_t i = 0; i < Config::HOP_CHANNEL_MAX; ++i) {
      if (used[i]) continue;
      const auto& r = scanner_.results[i];
      if (r.timestamp == 0 || r.generation != scannerGeneration_) continue;
      if (r.occupancyPercent < bestLoad ||
          (r.occupancyPercent == bestLoad && r.rssiAvgDbm < bestRssi)) {
        best = i;
        bestLoad = r.occupancyPercent;
        bestRssi = r.rssiAvgDbm;
      }
    }
    if (best == Config::HOP_CHANNEL_MAX) break;
    used[best] = true;
    channels[out++] = static_cast<uint8_t>(best);
  }
  xSemaphoreGive(mutex_);
  return out;
}

void LoRaManager::addSosHistory(uint8_t event, uint32_t peer) {
  StateLock lock(gState);
  if (!lock.ok()) return;
  auto& e = gState.sosHistory[gState.sosHistoryNext];
  e.timestamp = gState.gps.timeValid ? gState.gps.utcEpoch : millis();
  e.seq = sosSeq_.load(std::memory_order_acquire);
  e.event = event;
  e.peer = peer;
  gState.sosHistoryNext = (gState.sosHistoryNext + 1) % RuntimeState::SOS_HISTORY_SIZE;
  if (gState.sosHistoryCount < RuntimeState::SOS_HISTORY_SIZE) ++gState.sosHistoryCount;
}

void LoRaManager::serviceSosRetry() {
  if (!sosAwaitingAck_ || sosPacket_.isEmpty()) return;
  const uint32_t now = millis();
  if (now - sosSentMs_ < Config::SOS_REPEAT_MS) return;
  if (sosRetryCount_ >= Config::SOS_MAX_RETRIES &&
      now - sosSentMs_ >= Config::SOS_ESCALATION_DELAY_MS) {
    sosAwaitingAck_ = false;
    addSosHistory(2);
    StateLock lock(gState);
    if (lock.ok()) {
      gState.sosEscalated = true;
      gState.sosEscalatedMs = now;
      gState.lastError = "SOS ACK timeout; escalation active";
    }
    return;
  }
  String retryPacket = sosPacket_;
  bool hopEnabled = false;
  {
    StateLock lock(gState);
    if (lock.ok()) hopEnabled = gState.hopEnabled && gState.hopChannelCount > 0;
  }
  if (hopEnabled && retryPacket.length() >= PACKET_HEADER_V2 &&
      static_cast<uint8_t>(retryPacket[1]) == Config::LORA_PROTOCOL_VERSION) {
    uint8_t plain[Config::LORA_MAX_PACKET] = {};
    uint8_t type = 0, ttl = 0, hop = 0;
    uint16_t seq = 0;
    uint32_t source = 0;
    size_t plainLen = 0;
    if (decryptPacket(retryPacket, type, seq, source, ttl, plain,
                      sizeof(plain), plainLen)) {
      uint8_t routed[Config::LORA_MAX_PACKET] = {};
      const size_t routedLen = addRouteExtension(plain, plainLen, 0, 0,
                                                  routed, sizeof(routed));
      String rebuilt;
      const uint8_t newHop = computeHopIndex(hopFrame_);
      if (routedLen && encryptPacketV3(routed, routedLen, type, seq, newHop,
                                       currentEpochSec(), rebuilt))
        retryPacket = rebuilt;
    }
  }
  if (transmit(retryPacket, true)) {
    sosPacket_ = retryPacket;
    ++sosRetryCount_;
    sosSentMs_ = now;
    StateLock lock(gState);
    if (lock.ok()) gState.sosRetries = sosRetryCount_;
  }
}

bool LoRaManager::sendTextAck(uint16_t ackedSeq, uint32_t ackedSourceId, uint8_t hopIndex) {
  uint8_t payload[6] = {};
  payload[0] = static_cast<uint8_t>(ackedSeq & 0xFF);
  payload[1] = static_cast<uint8_t>(ackedSeq >> 8);
  memcpy(payload + 2, &ackedSourceId, sizeof(ackedSourceId));
  uint16_t seq = 0;
  if (!nextTxSequence(seq)) return false;
  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const size_t routedLen = addRouteExtension(
      payload, sizeof(payload), ackedSourceId, 0, routed, sizeof(routed));
  String packet;
  const uint32_t epochSec = currentEpochSec();
  if (!routedLen ||
      !encryptPacketV3(routed, routedLen, Config::LORA_TYPE_TEXT_ACK, seq, hopIndex, epochSec, packet)) return false;
  return transmit(packet, true);
}

void LoRaManager::handleTextAckPayload(const uint8_t* payload, size_t len) {
  if (!payload || len != 6) return;
  uint16_t ackedSeq = static_cast<uint16_t>(payload[0]) | (static_cast<uint16_t>(payload[1]) << 8);
  uint32_t ackedSource = 0;
  memcpy(&ackedSource, payload + 2, sizeof(ackedSource));
  if (!textStateMutex_ ||
      xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;
  const bool match = ackedSource == sourceId_ && textAwaitingAck_ &&
                     ackedSeq == textPendingSeq_;
  if (match) {
    textAwaitingAck_ = false;
    textAcked_ = true;
  }
  xSemaphoreGive(textStateMutex_);
}

bool LoRaManager::validateTextAckHop() const {
  if (!textAwaitingAck_ || textPendingPacket_.isEmpty()) return true;
  const bool hopEnabled = [&]() {
    StateLock lock(gState);
    return lock.ok() && gState.hopEnabled && gState.hopChannelCount > 0;
  }();
  const uint8_t version = textPendingPacket_.length() > 1
      ? static_cast<uint8_t>(textPendingPacket_[1]) : 0;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  if (version == Config::LORA_PROTOCOL_VERSION_ECDH) {
    if (textPendingPacket_.length() < PACKET_HEADER_V5) return false;
    return !hopEnabled ||
           static_cast<uint8_t>(textPendingPacket_[14]) ==
               computeHopIndex(hopFrame_);
  }
#endif
  if (!hopEnabled) return version != LORA_PROTOCOL_VERSION_HOP;
  if (version != LORA_PROTOCOL_VERSION_HOP ||
      textPendingPacket_.length() < PACKET_HEADER_V3) return false;
  return static_cast<uint8_t>(textPendingPacket_[14]) == computeHopIndex(hopFrame_);
}

void LoRaManager::serviceTextRetry() {
  String packet;
  uint32_t sentMs = 0;
  uint8_t retryCount = 0;
  {
    if (!textStateMutex_ ||
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (!textAwaitingAck_ || textPendingPacket_.isEmpty() ||
        millis() - textSentMs_ < Config::LORA_FRAGMENT_ACK_TIMEOUT_MS) {
      xSemaphoreGive(textStateMutex_);
      return;
    }
    if (textRetryCount_ >= Config::SOS_MAX_RETRIES) {
      textAwaitingAck_ = false;
      textAcked_ = false;
      xSemaphoreGive(textStateMutex_);
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "Text ACK timeout";
      return;
    }

    // V3 packets authenticate the hop index. A retry must be rebuilt when the
    // hop frame has advanced; otherwise it can be transmitted on a channel
    // that no longer matches the receiver's current hopping slot.
    bool hopEnabled = false;
    {
      StateLock stateLock(gState);
      if (stateLock.ok())
        hopEnabled = gState.hopEnabled && gState.hopChannelCount > 0;
    }
    if (hopEnabled) {
      uint8_t plain[Config::LORA_MAX_PACKET] = {};
      uint8_t type = 0, ttl = 0, hop = 0;
      uint16_t seq = 0;
      uint32_t source = 0, epoch = 0;
      size_t plainLen = 0;
      bool rebuiltNeeded = false;
      uint8_t newHop = computeHopIndex(hopFrame_);
      if (textPendingPacket_.length() >= PACKET_HEADER_V3 &&
          (static_cast<uint8_t>(textPendingPacket_[1]) == LORA_PROTOCOL_VERSION_HOP
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
           || static_cast<uint8_t>(textPendingPacket_[1]) ==
                  Config::LORA_PROTOCOL_VERSION_ECDH
#endif
          )) {
        const uint8_t oldHop = static_cast<uint8_t>(textPendingPacket_[14]);
        rebuiltNeeded = oldHop != newHop;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
        if (rebuiltNeeded &&
            static_cast<uint8_t>(textPendingPacket_[1]) ==
                Config::LORA_PROTOCOL_VERSION_ECDH) {
          uint8_t keyEpochDelta = 0;
          if (!decryptPacketV5(textPendingPacket_, type, seq, source, ttl,
                               hop, epoch, keyEpochDelta, plain,
                               sizeof(plain), plainLen)) {
            textAwaitingAck_ = false;
            textAcked_ = false;
            xSemaphoreGive(textStateMutex_);
            return;
          }
        } else
#endif
        if (rebuiltNeeded &&
            !decryptPacketV3(textPendingPacket_, type, seq, source, ttl, hop,
                             epoch, plain, sizeof(plain), plainLen)) {
          textAwaitingAck_ = false;
          textAcked_ = false;
          xSemaphoreGive(textStateMutex_);
          return;
        }
      } else if (textPendingPacket_.length() >= PACKET_HEADER_V2 &&
                 static_cast<uint8_t>(textPendingPacket_[1]) == Config::LORA_PROTOCOL_VERSION) {
        rebuiltNeeded = true;
        if (!decryptPacket(textPendingPacket_, type, seq, source, ttl,
                           plain, sizeof(plain), plainLen)) {
          textAwaitingAck_ = false;
          textAcked_ = false;
          xSemaphoreGive(textStateMutex_);
          return;
        }
      }
      if (rebuiltNeeded) {
        // A V2 pending text packet has no hop index. Once hopping is enabled,
        // upgrade it to authenticated V3 rather than retrying it on channel 0.
        String rebuilt;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
        if (!encryptPacket(plain, plainLen, type, seq, rebuilt)) {
#else
        if (!encryptPacketV3(plain, plainLen, type, seq, newHop,
                             currentEpochSec(), rebuilt)) {
#endif
          xSemaphoreGive(textStateMutex_);
          return;
        }
        textPendingPacket_ = rebuilt;
      }
    } else if (textPendingPacket_.length() >= PACKET_HEADER_V3 &&
               (static_cast<uint8_t>(textPendingPacket_[1]) == LORA_PROTOCOL_VERSION_HOP
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
                || static_cast<uint8_t>(textPendingPacket_[1]) ==
                       Config::LORA_PROTOCOL_VERSION_ECDH
#endif
               )) {
      // Conversely, once hopping is disabled, re-authenticate the same origin
      // frame with a zero hop index. V5 remains V5 when ECDH is enabled.
      uint8_t plain[Config::LORA_MAX_PACKET] = {};
      uint8_t type = 0, ttl = 0, hop = 0;
      uint16_t seq = 0;
      uint32_t source = 0, epoch = 0;
      size_t plainLen = 0;
      bool decrypted = false;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
      if (static_cast<uint8_t>(textPendingPacket_[1]) ==
          Config::LORA_PROTOCOL_VERSION_ECDH) {
        uint8_t keyEpochDelta = 0;
        decrypted = decryptPacketV5(textPendingPacket_, type, seq, source, ttl,
                                    hop, epoch, keyEpochDelta, plain,
                                    sizeof(plain), plainLen);
      } else
#endif
      {
        decrypted = decryptPacketV3(textPendingPacket_, type, seq, source, ttl,
                                    hop, epoch, plain, sizeof(plain), plainLen);
      }
      if (!decrypted) {
        textAwaitingAck_ = false;
        textAcked_ = false;
        xSemaphoreGive(textStateMutex_);
        return;
      }
      String rebuilt;
      if (!encryptPacket(plain, plainLen, type, seq, rebuilt)) {
        xSemaphoreGive(textStateMutex_);
        return;
      }
      textPendingPacket_ = rebuilt;
    }
    if (!validateTextAckHop()) {
      textAwaitingAck_ = false;
      textAcked_ = false;
      xSemaphoreGive(textStateMutex_);
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "Text retry hop validation failed";
      return;
    }
    packet = textPendingPacket_;
    sentMs = textSentMs_;
    retryCount = textRetryCount_;
    xSemaphoreGive(textStateMutex_);
  }

  if (transmit(packet, true)) {
    if (textStateMutex_ &&
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (textAwaitingAck_ && textPendingPacket_ == packet &&
          textRetryCount_ == retryCount && textSentMs_ == sentMs) {
        ++textRetryCount_;
        textSentMs_ = millis();
      }
      xSemaphoreGive(textStateMutex_);
    }
  }
}

void LoRaManager::handleSosAckPayload(const uint8_t* payload, size_t len) {
  if (!payload || len != 6) return;
  uint16_t ackedSeq = static_cast<uint16_t>(payload[0]) |
                      (static_cast<uint16_t>(payload[1]) << 8);
  uint32_t ackedSource = 0;
  memcpy(&ackedSource, payload + 2, sizeof(ackedSource));
  if (!sosAwaitingAck_ || ackedSeq != sosSeq_.load(std::memory_order_acquire) || ackedSource != sourceId_) return;
  sosAwaitingAck_ = false;
  StateLock lock(gState);
  if (lock.ok()) {
    gState.sosAcked = true;
    gState.sosLastAckMs = millis();
    gState.sosAckedBy = ackedSource;
    gState.sos = false;
  }
  addSosHistory(1, ackedSource);
}

bool LoRaManager::sendSosAck(uint16_t ackedSeq, uint32_t ackedSourceId) {
  if (!ready_) return false;
  uint8_t payload[6];
  payload[0] = static_cast<uint8_t>(ackedSeq & 0xFF);
  payload[1] = static_cast<uint8_t>(ackedSeq >> 8);
  memcpy(payload + 2, &ackedSourceId, sizeof(ackedSourceId));
  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const size_t routedLen = addRouteExtension(
      payload, sizeof(payload), ackedSourceId, 0, routed, sizeof(routed));
  String packet;
  uint16_t seq = 0;
  if (!nextTxSequence(seq)) return false;
  if (!routedLen ||
      !encryptPacket(routed, routedLen, Config::LORA_TYPE_SOS_ACK, seq, packet))
    return false;
  return transmit(packet, true);
}

void LoRaManager::suspendForLoRaWAN() {
  if (suspendedForLoRaWAN_.exchange(true, std::memory_order_acq_rel)) return;
  if (!mutex_) return;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;
  ready_ = false;
  {
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (spiLock.ok()) (void)radio_.sleep();
  }
  StateLock lock(gState);
  if (lock.ok()) gState.loraReady = false;
  xSemaphoreGive(mutex_);
}

bool LoRaManager::resumeFromLoRaWAN() {
  if (!mutex_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  bool ok = false;
  {
    SpiLock spiLock(pdMS_TO_TICKS(500));
    if (spiLock.ok()) {
      const int16_t st = radio_.begin(
          gConfig.loraFreqMHz, gConfig.loraBwKHz, gConfig.loraSf,
          gConfig.loraCr, gConfig.loraSyncWord, effectiveTxPowerDbm(),
          Config::LORA_PREAMBLE, Config::LORA_TCXO_VOLTAGE);
      if (st == RADIOLIB_ERR_NONE) {
        radio_.setPacketReceivedAction(onDio1);
        ok = radio_.startReceive() == RADIOLIB_ERR_NONE;
      }
    }
  }
  ready_ = ok;
  suspendedForLoRaWAN_.store(false, std::memory_order_release);
  StateLock lock(gState);
  if (lock.ok()) {
    gState.loraReady = ok;
    if (!ok) gState.lastError = "Failed to resume P2P radio after LoRaWAN";
  }
  xSemaphoreGive(mutex_);
  return ok;
}

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
bool LoRaManager::processEcdhBeacon(uint32_t sourceId, uint32_t packetEpochSec,
                                    const uint8_t* payload, size_t len) {
  if (sourceId == 0 || packetEpochSec == 0 || !payload ||
      len != LoRaEcdhRekey::BEACON_BYTES)
    return false;

  LoRaEcdhRekey::Beacon beacon{};
  if (!LoRaEcdhRekey::decodeBeacon(payload, len, beacon))
    return false;

  const uint32_t localEpochSec = currentEpochSec();
  if (!LoRaEcdhRekey::authenticatedBeaconValid(
          beacon, sourceId, packetEpochSec, localEpochSec))
    return false;

  if (localEpochSec == 0) {
    // DECISION: the C3 has no absolute RTC/GPS source, so an authenticated
    // S3 beacon is its temporary epoch authority. Never move backwards and
    // never accept a jump larger than one epoch.
    if (ecdhAuthoritativeEpochSec_ != 0) {
      const uint32_t localEpoch =
          LoRaEcdhRekey::epochNumber(ecdhAuthoritativeEpochSec_);
      const uint32_t peerEpoch = LoRaEcdhRekey::epochNumber(beacon.epochSec);
      const uint32_t delta = localEpoch >= peerEpoch
          ? localEpoch - peerEpoch
          : peerEpoch - localEpoch;
      if (delta > 1U) return false;
      if (peerEpoch > localEpoch)
        ecdhAuthoritativeEpochSec_ = beacon.epochSec;
    } else {
      ecdhAuthoritativeEpochSec_ = beacon.epochSec;
    }
  }

  // DECISION: the existing authenticated V3 HMAC is the trust anchor.
  // The authenticated sourceId and complete beacon payload bind the static
  // public key to the already-authenticated node identity.
  size_t selected = ECDH_PEER_CACHE_SIZE;
  for (size_t i = 0; i < ECDH_PEER_CACHE_SIZE; ++i) {
    if (ecdhPeers_[i].valid && ecdhPeers_[i].sourceId == sourceId) {
      selected = i;
      break;
    }
    if (selected == ECDH_PEER_CACHE_SIZE && !ecdhPeers_[i].valid)
      selected = i;
  }
  if (selected == ECDH_PEER_CACHE_SIZE) {
    uint32_t oldestAge = 0;
    for (size_t i = 0; i < ECDH_PEER_CACHE_SIZE; ++i) {
      const uint32_t age = millis() - ecdhPeers_[i].lastSeenMs;
      if (i == 0 || age > oldestAge) {
        oldestAge = age;
        selected = i;
      }
    }
  }

  EcdhPeerState& peer = ecdhPeers_[selected];
  if (peer.valid &&
      memcmp(peer.staticPublic, beacon.staticPublic,
             LoRaEcdhRekey::PUBLIC_KEY_BYTES) != 0) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "ECDH static key changed";
    return false;
  }

  peer.sourceId = sourceId;
  peer.epochSec = beacon.epochSec;
  memcpy(peer.ephemeralPublic, beacon.ephemeralPublic,
         LoRaEcdhRekey::PUBLIC_KEY_BYTES);
  memcpy(peer.staticPublic, beacon.staticPublic,
         LoRaEcdhRekey::PUBLIC_KEY_BYTES);
  peer.lastSeenMs = millis();
  peer.valid = true;

  if (ecdhKeyMaterial_.hasEphemeralKey() &&
      ecdhKeyMaterial_.ephemeralEpoch() ==
          LoRaEcdhRekey::epochNumber(beacon.epochSec)) {
    if (!ecdhKeyMaterial_.deriveSessionKey(
            beacon.ephemeralPublic, sourceId_, sourceId, beacon.epochSec)) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "ECDH beacon key derivation failed";
      return false;
    }
  }
  return true;
}

#endif

bool LoRaManager::begin() {
  instance_ = this;
  mutex_ = xSemaphoreCreateMutex();
  seqMutex_ = xSemaphoreCreateMutex();
  textStateMutex_ = xSemaphoreCreateMutex();
  captureMutex_ = xSemaphoreCreateMutex();
  forwardQueue_ = xQueueCreateStatic(FORWARD_QUEUE_DEPTH, sizeof(ForwardPacket),
                                     forwardQueueStorage_, &forwardQueueStruct_);
  sourceId_ = sourceIdFromCallsign(gConfig.callsign);
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  // DECISION: fail closed if ECDH key material cannot be initialized while
  // the feature flag is enabled; do not start a partially initialized node.
  if (!ecdhKeyMaterial_.begin(currentEpochSec())) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "ECDH key material init failed";
    return false;
  }
  const uint32_t bootEpochSec = currentEpochSec();
  ecdhKeyEpoch_ = bootEpochSec != 0
      ? LoRaEcdhRekey::epochNumber(bootEpochSec) : 0;
  ecdhActive_ = ecdhKeyMaterial_.hasEphemeralKey();
#endif
  if (rtcRadioState.magic == RTC_RADIO_MAGIC && rtcRadioState.crc == stateCrc(rtcRadioState)) {
    hopFrame_ = rtcRadioState.hopFrame;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
    ecdhKeyEpoch_ = rtcRadioState.ecdhKeyEpoch;
    ecdhActive_ = rtcRadioState.ecdhActive;
#endif
    sosSeq_.store(rtcRadioState.sosSeq, std::memory_order_release);
    sosRetryCount_ = rtcRadioState.sosRetryCount;
    sosAwaitingAck_ = rtcRadioState.sosAwaitingAck;
    if (sosAwaitingAck_ && rtcRadioState.sosPacketLen > 0 &&
        rtcRadioState.sosPacketLen <= Config::LORA_MAX_PACKET) {
      sosPacket_ = String(reinterpret_cast<const char*>(rtcRadioState.sosPacket), rtcRadioState.sosPacketLen);
      sosSentMs_ = millis() - min<uint32_t>(rtcRadioState.sosElapsedMs, 0x7FFFFFFFU);
    }
  }
  if (!mutex_ || !seqMutex_ || !textStateMutex_ || !captureMutex_ ||
      !forwardQueue_ || !reserveTxSequenceBlock()) return false;
  const bool replayStoreOk = replayStore_.begin();
  if (!replayStoreOk) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "Replay persistence unavailable";
  }
  const bool rfDetectorOk = rfDetector_.begin();
  if (!rfDetectorOk) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "MAX2016 detector init failed; using LoRa RSSI fallback";
  }
  (void)loadForwardQueue();
  (void)loadMessageHistory();
  (void)loadFragmentRx();
  (void)loadReplayState();
  (void)loadScheduledMessages();

  SpiLock spiLock(pdMS_TO_TICKS(1000));
  if (!spiLock.ok()) return false;

  int16_t st = radio_.begin(
      gConfig.loraFreqMHz, gConfig.loraBwKHz, gConfig.loraSf,
      gConfig.loraCr, gConfig.loraSyncWord, effectiveTxPowerDbm(),
      Config::LORA_PREAMBLE, Config::LORA_TCXO_VOLTAGE);

  if (st != RADIOLIB_ERR_NONE) {
    hardResetRadio();
    ready_ = false;
    StateLock lock(gState);
    if (lock.ok()) {
      gState.loraReady = false;
      gState.lastError = "SX1262 init failed: " + String(st);
    }
    return false;
  }

  radio_.setPacketReceivedAction(onDio1);
  st = radio_.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    ready_ = false;
    StateLock lock(gState);
    if (lock.ok()) {
      gState.loraReady = false;
      gState.lastError = "SX1262 RX failed: " + String(st);
    }
    return false;
  }

  ready_ = true;
  StateLock lock(gState);
  if (lock.ok()) gState.loraReady = true;
  return true;
}


void LoRaManager::serviceVoiceReorder() {
  if (!haveVoiceRxSequence_ || isPttOrRecording()) return;
  for (;;) {
    const uint16_t want = static_cast<uint16_t>(lastVoiceRxSequence_ + 1);
    VoiceRxSlot* selected = nullptr;
    for (auto& slot : voiceRx_)
      if (slot.used && slot.seq == want) { selected = &slot; break; }
    if (!selected) break;
    const bool played = audio.playVoiceFrame(selected->data, sizeof(selected->data));
    selected->used = false;
    if (!played) {
      StateLock lock(gState);
      if (lock.ok()) ++gState.voiceDrops;
      break;
    }
    lastVoiceRxSequence_ = want;
    StateLock lock(gState);
    if (lock.ok()) {
      ++gState.voiceRxPackets;
      gState.rxActive = true;
      gState.rxActivityMs = millis();
    }
  }

  const uint32_t now = millis();
  VoiceRxSlot* oldest = nullptr;
  uint16_t oldestSeq = 0;
  for (auto& slot : voiceRx_) {
    if (!slot.used) continue;
    if (!oldest || static_cast<int16_t>(slot.seq - oldestSeq) < 0) {
      oldest = &slot;
      oldestSeq = slot.seq;
    }
  }
  if (oldest && now - oldest->receivedMs >= Config::VOICE_REORDER_HOLD_MS) {
    const uint16_t want = static_cast<uint16_t>(lastVoiceRxSequence_ + 1);
    const uint16_t skipped = static_cast<uint16_t>(oldestSeq - want);
    if (skipped > 0 && skipped < 0x8000U) {
      StateLock lock(gState);
      if (lock.ok()) gState.voiceRxLost += skipped;
      lastVoiceRxSequence_ = static_cast<uint16_t>(oldestSeq - 1);
      serviceVoiceReorder();
    }
  }
}

void LoRaManager::task() {
  if (!mutex_ || suspendedForLoRaWAN_.load(std::memory_order_acquire)) return;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  uint32_t ecdhEpochSec = currentEpochSec();
  if (ecdhEpochSec == 0)
    ecdhEpochSec = ecdhAuthoritativeEpochSec_;

  if (ecdhEpochSec != 0 && !ecdhKeyMaterial_.ensureEphemeral(ecdhEpochSec)) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "ECDH ephemeral key rotation failed";
  } else if (ecdhEpochSec != 0 && ecdhKeyMaterial_.hasEphemeralKey()) {
    for (const auto& peer : ecdhPeers_) {
      if (!peer.valid ||
          LoRaEcdhRekey::epochNumber(peer.epochSec) !=
              ecdhKeyMaterial_.ephemeralEpoch())
        continue;

      uint8_t keyProbe[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
      const bool haveKey = ecdhKeyMaterial_.getSessionKey(
          peer.sourceId, peer.epochSec, keyProbe);
      mbedtls_platform_zeroize(keyProbe, sizeof(keyProbe));
      if (!haveKey &&
          !ecdhKeyMaterial_.deriveSessionKey(
              peer.ephemeralPublic, sourceId_, peer.sourceId,
              peer.epochSec)) {
        StateLock lock(gState);
        if (lock.ok()) gState.lastError = "ECDH beacon key derivation failed";
      }
    }
  }
#endif
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, 0);
  if (!radioGuard.ok()) return;
  if (adrEnabled_ && millis() - lastAdrMs_ >= Config::ADR_REEVALUATE_MS) {
    lastAdrMs_ = millis();
    serviceAdr();
  }
  serviceScheduledMessages();
  if (messageHistoryDirty_ && storage.ready()) {
    if (persistMessageHistory()) messageHistoryDirty_ = false;
  }
  if (fragmentRxDirty_ && storage.ready()) (void)persistFragmentRx();

  if (scanner_.active) {
    const uint32_t now = millis();
    if (now - scanner_.lastSampleMs < scanner_.dwellMs) return;
    if (xSemaphoreTake(mutex_, 0) != pdTRUE) return;
    const uint8_t index = scanner_.index;
    const float freq = Config::LORA_MIN_FREQ_MHZ +
                       static_cast<float>(index) * Config::HOP_CHANNEL_STEP_MHZ;
    int16_t scanSt = RADIOLIB_ERR_UNKNOWN;
    int16_t rxSt = RADIOLIB_ERR_NONE;
    int16_t rssi = -127;
    float snr = -20.0f;
    {
      SpiLock spiLock(pdMS_TO_TICKS(20));
      if (spiLock.ok() && freq <= Config::LORA_MAX_FREQ_MHZ &&
          radio_.setFrequency(freq) == RADIOLIB_ERR_NONE) {
        scanSt = radio_.scanChannel();
        rssi = static_cast<int16_t>(radio_.getRSSI());
        snr = radio_.getSNR();
        rxSt = radio_.startReceive();
      }
    }
    if (index < Config::SCANNER_MAX_CHANNELS) {
      auto& r = scanner_.results[index];
      r.freqMHz = freq;
      r.rssiAvgDbm = rssi;
      r.rssiPeakDbm = max(r.rssiPeakDbm, rssi);
      r.snrDb = snr;
      r.occupancyPercent = lbtChannelBusy(scanSt) ? 100 : 0;
      r.preambleCount = lbtChannelBusy(scanSt) ? 1 : 0;
      r.timestamp = now;
      r.generation = scannerGeneration_;
      int32_t rssiSum = 0;
      uint16_t occupancySum = 0;
      uint8_t validCount = 0;
      for (const auto& sample : scanner_.results) {
        if (sample.timestamp == 0) continue;
        rssiSum += sample.rssiAvgDbm;
        occupancySum += sample.occupancyPercent;
        ++validCount;
      }
      const int16_t noiseFloor = validCount ? static_cast<int16_t>(rssiSum / validCount) : -127;
      const uint8_t occupancy = validCount ? static_cast<uint8_t>(occupancySum / validCount) : 0;
      const bool jammed = validCount >= 3 &&
          noiseFloor >= Config::LORA_JAM_RSSI_THRESHOLD_DBM &&
          occupancy >= Config::LORA_JAM_OCCUPANCY_THRESHOLD_PERCENT;
      StateLock jamLock(gState);
      if (jamLock.ok()) {
        gState.noiseFloorDbm = validCount ? noiseFloor : -127;
        gState.channelOccupancy = occupancy;
        gState.jammingDetected = jammed;
        if (jammed) gState.lastError = "Possible LoRa jamming detected";
      }
    }
    ++scanner_.index;
    if (scanner_.index >= Config::SCANNER_MAX_CHANNELS) {
      scanner_.index = 0;
      ++scanner_.sweepCount;
      {
        StateLock sweepLock(gState);
        if (sweepLock.ok()) gState.scannerSweepInProgress = false;
      }
      if (scanner_.mode == 1) scanner_.active = false;
      if (scanner_.active) scanner_.lastSampleMs = now;
    } else {
      scanner_.lastSampleMs = now;
    }
    if (!scanner_.active) {
      (void)retuneToChannel0Locked();
      StateLock lock(gState);
      if (lock.ok()) {
        gState.scannerActive = false;
        gState.scannerSweepInProgress = false;
        gState.scannerSweepCount = scanner_.sweepCount;
        gState.scannerLastSweepMs = now;
      }
    } else {
      StateLock lock(gState);
      if (lock.ok()) {
        gState.scannerSweepCount = scanner_.sweepCount;
        gState.scannerLastSweepMs = now;
      }
    }
    (void)rxSt;
    xSemaphoreGive(mutex_);
    return;
  }

  {
    bool hopEnabled = false;
    bool busy = false;
    {
      StateLock lock(gState);
      if (lock.ok()) {
        hopEnabled = gState.hopEnabled && gState.hopChannelCount > 0;
        busy = gState.ptt || gState.recording;
      }
    }
    if (hopEnabled && !busy) {
      const uint32_t now = millis();
      if (now - hopLastSyncMs_ >= Config::HOP_DWELL_MS) {
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return;
        if (legacyRxCounter_ >= Config::HOP_LEGACY_RX_EVERY) {
          legacyRxCounter_ = 0;
          const uint8_t idx = computeHopIndex(hopFrame_);
          if (!retuneToHopChannelLocked(idx)) {
            xSemaphoreGive(mutex_);
            StateLock lock(gState);
            if (lock.ok()) gState.lastError = "hop retune failed";
            return;
          }
          currentHopIndex_ = idx;
          ++hopFrame_;
        } else {
          (void)retuneToChannel0Locked();
          ++legacyRxCounter_;
        }
        xSemaphoreGive(mutex_);
        hopLastSyncMs_ = now;
      }
    }
  }

  (void)processPendingTx();
  serviceSosRetry();
  serviceTextRetry();
  serviceVoiceAckRetry();
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  {
    const uint32_t epochSec = currentEpochSec();
    if (epochSec != 0) {
      const uint32_t epoch = LoRaEcdhRekey::epochNumber(epochSec);
      if (!ecdhKeyMaterial_.hasEphemeralKey() ||
          ecdhKeyMaterial_.ephemeralEpoch() != epoch) {
        if (!ecdhKeyMaterial_.ensureEphemeral(epochSec)) {
          ecdhActive_ = false;
          StateLock lock(gState);
          if (lock.ok()) gState.lastError = "ECDH ephemeral regeneration failed";
        }
      }
      if (ecdhKeyMaterial_.hasEphemeralKey() &&
          ecdhKeyMaterial_.ephemeralEpoch() == epoch) {
        ecdhKeyEpoch_ = epoch;
        ecdhActive_ = true;
      }
    }
  }
#endif
  serviceNeighborBeacon();
  serviceVoiceReorder();

  bool ptt = false;
  {
    StateLock lock(gState);
    if (lock.ok()) ptt = gState.ptt;
  }
  if (ptt && millis() - lastVoiceTxMs_ >= Config::VOICE_FRAME_MS) {
    if (sendVoiceFrame()) lastVoiceTxMs_ = millis();
  }

  if (!ready_) {
    const uint32_t now = millis();
    if (now - lastRecoveryMs_ < 5000 ||
        xSemaphoreTake(mutex_, 0) != pdTRUE) {
      return;
    }
    lastRecoveryMs_ = now;

    int16_t beginSt = RADIOLIB_ERR_NONE;
    int16_t rxSt = RADIOLIB_ERR_NONE;
    ++radioRecoveryAttempts_;
    hardResetRadio();
    {
      SpiLock spiLock(pdMS_TO_TICKS(1000));
      if (spiLock.ok()) {
        beginSt = radio_.begin(
            gConfig.loraFreqMHz, gConfig.loraBwKHz, gConfig.loraSf,
            gConfig.loraCr, gConfig.loraSyncWord, gConfig.loraPowerDbm,
            Config::LORA_PREAMBLE, Config::LORA_TCXO_VOLTAGE);
        if (beginSt == RADIOLIB_ERR_NONE) {
          radio_.setPacketReceivedAction(onDio1);
          rxSt = radio_.startReceive();
        }
      } else {
        beginSt = -1;
      }
    }

    if (beginSt == RADIOLIB_ERR_NONE && rxSt == RADIOLIB_ERR_NONE) {
      radioRecoveryAttempts_ = 0;
      ready_ = true;
      StateLock lock(gState);
      if (lock.ok()) {
        gState.loraReady = true;
        gState.lastError = "";
      }
    }
    xSemaphoreGive(mutex_);
    return;
  }

  if (xSemaphoreTake(mutex_, 0) != pdTRUE) return;

  portENTER_CRITICAL(&irqMux_);
  const bool pending = irqCount_ != 0;
  if (pending) --irqCount_;
  portEXIT_CRITICAL(&irqMux_);

  if (!pending) {
    xSemaphoreGive(mutex_);
    if (!isPttOrRecording() &&
        static_cast<int32_t>(millis() - forwardRetryNotBeforeMs_) >= 0 &&
        millis() - lastForwardTxMs_ >= Config::LORA_FORWARD_RATE_LIMIT_MS) {
      ForwardPacket forward{};
      if (forwardQueue_ && xQueueReceive(forwardQueue_, &forward, 0) == pdPASS) {
        if (transmitForward(forward)) {
          lastForwardTxMs_ = millis();
        } else if (forward.ttl > 0 && forwardQueue_) {
          // A failed non-LBT attempt used to silently discard the dequeued
          // frame. Requeue it so transient radio recovery does not lose data.
          if (xQueueSend(forwardQueue_, &forward, 0) != pdPASS) {
            ++forwardDrops_;
            forwardLastDropMs_ = millis();
          }
          (void)persistForwardQueue();
        }
      }
    }
    return;
  }

  bool spiOk = false;
  int16_t readSt = RADIOLIB_ERR_NONE;
  int16_t rxSt = RADIOLIB_ERR_NONE;
  String msg;
  int16_t rssi = -127;
  float snr = -20.0f;

  {
    SpiLock spiLock(pdMS_TO_TICKS(20));
    if (spiLock.ok()) {
      spiOk = true;
      readSt = radio_.readData(msg);
      if (readSt == RADIOLIB_ERR_NONE) {
        rssi = static_cast<int16_t>(radio_.getRSSI());
        snr = radio_.getSNR();
      }
      rxSt = radio_.startReceive();
    }
  }

  // Never hold the SPI mutex while taking the global state mutex. Other
  // managers update state after releasing SPI, so this lock ordering avoids
  // a cross-task deadlock.
  if (spiOk && readSt == RADIOLIB_ERR_NONE) {

    uint8_t plain[220] = {};
    uint8_t type = 0;
    uint16_t seq = 0;
    uint32_t rxSourceId = 0;
    uint8_t rxTtl = 0;
    size_t plainLen = 0;
    const bool authenticated = decryptPacket(
        msg, type, seq, rxSourceId, rxTtl, plain, sizeof(plain), plainLen);
    uint8_t hopIndex = 0;
    uint32_t epochSec = 0;
    bool authenticatedV3 = false;
    bool authenticatedV5 = false;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
    uint8_t keyEpochDelta = 0;
#endif
    if (!authenticated) {
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
      authenticatedV5 = decryptPacketV5(
          msg, type, seq, rxSourceId, rxTtl, hopIndex, epochSec,
          keyEpochDelta, plain, sizeof(plain), plainLen);
#endif
      if (!authenticatedV5) {
        authenticatedV3 = decryptPacketV3(msg, type, seq, rxSourceId, rxTtl,
                                          hopIndex, epochSec, plain,
                                          sizeof(plain), plainLen);
      }
      if (authenticatedV5 || authenticatedV3) {
        currentHopIndex_ = hopIndex;
        if (!hopSyncGps_) {
          StateLock hopLock(gState);
          if (hopLock.ok() && gState.hopChannelCount > 0 &&
              hopIndex < gState.hopChannelCount) {
            // V3 carries the authoritative channel index. Seed the frame
            // counter so the next dwell advances from the channel actually
            // observed instead of drifting from a stale local counter.
            hopFrame_ = hopIndex;
          }
        }
        hopLastSyncMs_ = millis();
      }
    }
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
    const bool ecdhPolicyAccepted =
        authenticatedV5 ||
        (authenticatedV3 &&
         type == Config::LORA_TYPE_NEIGHBOR_BEACON &&
         processEcdhBeacon(rxSourceId, epochSec, plain, plainLen));
    if ((authenticated || authenticatedV3 || authenticatedV5) &&
        !ecdhPolicyAccepted) {
      StateLock lock(gState);
      if (lock.ok()) {
        gState.lastError = (authenticatedV3 &&
                            type == Config::LORA_TYPE_NEIGHBOR_BEACON)
            ? "ECDH beacon rejected"
            : "ECDH peer unsupported";
      }
    }
#else
    const bool ecdhPolicyAccepted = true;
#endif
    const bool rxAuthenticated =
        (authenticated || authenticatedV3 || authenticatedV5) &&
        rxTtl > 0 && rxTtl <= Config::LORA_INITIAL_TTL &&
        ecdhPolicyAccepted;
    // Capture the exact over-the-air frame after authentication has been
    // attempted, but before any routing/decryption buffer is mutated.
    (void)capturePacket(msg, rssi, snr, rxAuthenticated);
    const bool isV2 = authenticated &&
                     static_cast<uint8_t>(msg[1]) == Config::LORA_PROTOCOL_VERSION;
    const bool isV4 = authenticated &&
                     static_cast<uint8_t>(msg[1]) == Config::LORA_PROTOCOL_VERSION_GCM;
    const bool isV3 = authenticatedV3;
    const bool isV5 = authenticatedV5;
    uint32_t routeDestination = 0;
    uint32_t routeNextHop = 0;
    uint32_t routePreviousHop = 0;
    uint8_t routeHopCount = 0;
    size_t routeOffset = 0;
    const bool routePrefixPresent = rxAuthenticated && plainLen >= 2 &&
        plain[0] == ROUTE_EXT_MAGIC &&
        (plain[1] == ROUTE_EXT_VERSION || plain[1] == ROUTE_EXT_VERSION_V1);
    const bool hasRouteExtension = rxAuthenticated &&
        parseRouteExtension(plain, plainLen, routeDestination, routeNextHop,
                            routePreviousHop, routeHopCount, routeOffset);
    const bool malformedRouteExtension = routePrefixPresent && !hasRouteExtension;
    const uint8_t* appPayload = hasRouteExtension ? plain + routeOffset : plain;
    const size_t appPayloadLen = hasRouteExtension ? plainLen - routeOffset : plainLen;
    const bool addressedToUs = !malformedRouteExtension &&
                               (!hasRouteExtension || routeDestination == 0 ||
                                routeDestination == sourceId_);
    const bool nextHopIsUs = !hasRouteExtension || routeNextHop == 0 ||
                             routeNextHop == sourceId_;
    const bool duplicateV2 = (isV2 || isV3 || isV4 || isV5) &&
        seenDedup(rxSourceId, seq, type, hashPayload(plain, plainLen),
                  (isV3 || isV5) ? epochSec : 0);
    const uint32_t immediatePeer = (hasRouteExtension && routePreviousHop != 0)
        ? routePreviousHop : rxSourceId;
    if (rxAuthenticated) updateNeighborMetric(immediatePeer, rssi, snr);
    if (rxAuthenticated && nextHopIsUs &&
        (type == Config::LORA_TYPE_TEXT_ACK ||
         type == Config::LORA_TYPE_SOS_ACK ||
         type == Config::LORA_TYPE_VOICE_ACK))
      recordNeighborTxResult(immediatePeer, true);
    if (hasRouteExtension && routePreviousHop != 0)
      learnRoute(rxSourceId, routePreviousHop, rssi, snr);
    const bool isForwardable =
        rxAuthenticated && (isV2 || isV3 || isV4 || isV5) &&
        !duplicateV2 && !malformedRouteExtension;
    bool pttOrRecording = false;
    {
      StateLock stateLock(gState);
      if (stateLock.ok()) pttOrRecording = gState.ptt || gState.recording;
    }
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_NEIGHBOR_BEACON && !duplicateV2 &&
        appPayloadLen >= 8 && appPayload[0] == BEACON_MAGIC) {
      updateNeighborMetric(rxSourceId, rssi, snr);
    }
    bool textPayloadValid = false;
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_TEXT &&
        appPayloadLen > 0 && !duplicateV2) {
      const bool looksLikeFragment =
          appPayloadLen >= 2 && appPayload[0] == FRAGMENT_MAGIC &&
          appPayload[1] == Config::LORA_FRAGMENT_VERSION;
      if (looksLikeFragment) {
        // A fragment-shaped payload must pass fragment validation. Do not
        // ACK malformed fragments or reinterpret them as ordinary text.
        textPayloadValid = handleTextFragment(rxSourceId, appPayload, appPayloadLen);
      } else {
        char text[Config::LORA_MAX_PACKET + 1] = {};
        memcpy(text, appPayload, appPayloadLen);
        text[appPayloadLen] = '\0';
        addMessageHistory(rxSourceId, text);
        textPayloadValid = true;
      }
    } else if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_TEXT &&
               appPayloadLen > 0 && duplicateV2) {
      // Duplicates are still valid authenticated text and may need an ACK.
      textPayloadValid = true;
    }
    if (rxAuthenticated && !malformedRouteExtension && addressedToUs &&
        nextHopIsUs && (type == Config::LORA_TYPE_TEXT || type == Config::LORA_TYPE_FRAG_DATA) && textPayloadValid) {
      textAckSeq_ = seq;
      textAckSourceId_ = rxSourceId;
      textAckHopIndex_ = (isV3 || isV5) ? hopIndex : 0;
      textAckPending_ = true;
    }
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_SOS && appPayloadLen > 0) {
      // Re-ACK an authenticated duplicate so an ACK lost on-air does not
      // force the sender into a false SOS escalation. Do not duplicate the
      // user-visible history entry.
      if (!duplicateV2) {
        char text[Config::LORA_MAX_PACKET + 1] = {};
        memcpy(text, appPayload, appPayloadLen);
        text[appPayloadLen] = '\0';
        addMessageHistory(rxSourceId, text);
      }
      sosAckPendingSeq_ = seq;
      sosAckPendingSourceId_ = rxSourceId;
      sosAckPending_ = true;
    }
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_SOS_ACK && !duplicateV2) {
      handleSosAckPayload(appPayload, appPayloadLen);
    }
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_TEXT_ACK) {
      handleTextAckPayload(appPayload, appPayloadLen);
    }
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_VOICE && appPayloadLen == 168 &&
        appPayload[0] == 0x56 && appPayload[1] == 1 &&
        (static_cast<uint16_t>(appPayload[4]) | (static_cast<uint16_t>(appPayload[5]) << 8)) == seq &&
        appPayload[2] == Config::VOICE_FRAME_MS &&
        rssi >= Config::VOICE_RSSI_THRESHOLD_DBM &&
        snr >= Config::VOICE_SNR_THRESHOLD_DB && !pttOrRecording) {
      if (!haveVoiceRxSequence_) {
        lastVoiceRxSequence_ = static_cast<uint16_t>(seq - 1);
        haveVoiceRxSequence_ = true;
      }
      const uint16_t expected = static_cast<uint16_t>(lastVoiceRxSequence_ + 1);
      const int16_t distance = static_cast<int16_t>(seq - expected);
      if (distance >= 0 &&
          distance < static_cast<int16_t>(Config::VOICE_REORDER_BUFFER_SIZE)) {
        bool alreadyQueued = false;
        for (auto& slot : voiceRx_)
          if (slot.used && slot.seq == seq) { alreadyQueued = true; break; }
        if (!duplicateV2 && !alreadyQueued) {
          VoiceRxSlot* freeSlot = nullptr;
          for (auto& slot : voiceRx_)
            if (!slot.used) { freeSlot = &slot; break; }
          if (freeSlot) {
            freeSlot->used = true;
            freeSlot->seq = seq;
            freeSlot->receivedMs = millis();
            memcpy(freeSlot->data, appPayload, sizeof(freeSlot->data));
          } else {
            StateLock lossLock(gState);
            if (lossLock.ok()) ++gState.voiceRxLost;
          }
        }
      } else if (distance >= 0) {
        StateLock lossLock(gState);
        if (lossLock.ok()) gState.voiceRxLost += static_cast<uint32_t>(distance);
        lastVoiceRxSequence_ = static_cast<uint16_t>(seq - 1);
      }
      if (!voiceRxAckInitialized_ || voiceRxAckSourceId_ != rxSourceId) {
        voiceRxAckInitialized_ = true;
        voiceRxAckSourceId_ = rxSourceId;
        voiceRxAckBase_ = static_cast<uint16_t>(seq - 1U);
        voiceRxAckBitmap_ = 0;
      }
      const uint16_t ackDistance = static_cast<uint16_t>(seq - voiceRxAckBase_);
      if (ackDistance == 0) {
        // Duplicate already covered by the cumulative ACK.
      } else if (ackDistance <= 8U) {
        voiceRxAckBitmap_ |= static_cast<uint8_t>(1U << (ackDistance - 1U));
        while (voiceRxAckBitmap_ & 0x01U) {
          ++voiceRxAckBase_;
          voiceRxAckBitmap_ >>= 1;
        }
      } else if (ackDistance < 0x8000U) {
        // Beyond the SACK window: keep the current ACK state. Do not falsely
        // cumulatively ACK frames that were not received.
      }
      voiceAckPendingSeq_ = seq;
      voiceAckPendingSourceId_ = rxSourceId;
      voiceAckPendingRssi_ = rssi;
      voiceAckPendingSnr_ = snr;
      voiceAckPending_ = true;
      serviceVoiceReorder();
    }

    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_VOICE_ACK && !duplicateV2) {
      handleVoiceAckPayload(rxSourceId, appPayload, appPayloadLen);
    }

    const bool routeForwardAllowed = !hasRouteExtension ||
        routeAllowsForward(routeDestination, routeNextHop, routePreviousHop);
    if (isForwardable && routeForwardAllowed && rxSourceId != sourceId_ &&
        rxTtl > 1 && plainLen > 0) {
      (void)enqueueForward(
          type, seq, rxSourceId, rxTtl, plain, plainLen,
          isV5 ? Config::LORA_PROTOCOL_VERSION_ECDH :
          (isV3 ? LORA_PROTOCOL_VERSION_HOP : Config::LORA_PROTOCOL_VERSION));
    }

    if (rxAuthenticated) logPacket(false, type, seq, rxSourceId, rssi, snr, rxTtl);
    StateLock rxState(gState);
    if (rxState.ok()) {
      gState.loraRssi = rssi;
      gState.loraSnr = snr;
      const size_t idx = gState.radioHistoryNext;
      gState.rssiHistory[idx] = rssi;
      gState.snrHistory[idx] = snr;
      gState.radioHistoryMs[idx] = millis();
      gState.radioHistoryNext = (idx + 1) % RuntimeState::RADIO_HISTORY_SIZE;
      if (gState.radioHistoryCount < RuntimeState::RADIO_HISTORY_SIZE) ++gState.radioHistoryCount;
    }
  }
  StateLock lock(gState);
  if (lock.ok()) {
    if (!spiOk) {
      gState.rxDrops++;
    } else if (readSt == RADIOLIB_ERR_NONE) {
      gState.rxPackets++;
      if (!rxAuthenticated) gState.lastMessage = "RX: authentication failed";
    } else {
      gState.rxDrops++;
    }

    if (spiOk && rxSt != RADIOLIB_ERR_NONE) {
      gState.rxDrops++;
      ready_ = false;
      gState.loraReady = false;
      gState.lastError = "SX1262 RX restart failed: " + String(rxSt);
    }
  }
  xSemaphoreGive(mutex_);

  if (sosAckPending_) {
    const uint16_t ackSeq = sosAckPendingSeq_;
    const uint32_t ackSource = sosAckPendingSourceId_;
    sosAckPending_ = false;
    (void)sendSosAck(ackSeq, ackSource);
  }

  if (voiceAckPending_) {
    const uint16_t ackSeq = voiceAckPendingSeq_;
    const uint32_t ackSource = voiceAckPendingSourceId_;
    const int16_t ackRssi = voiceAckPendingRssi_;
    const float ackSnr = voiceAckPendingSnr_;
    voiceAckPending_ = false;
    (void)sendVoiceAck(ackSeq, ackSource, ackRssi, ackSnr);
  }

  if (textAckPending_) {
    const uint16_t ackSeq = textAckSeq_;
    const uint32_t ackSource = textAckSourceId_;
    const uint8_t ackHop = textAckHopIndex_;
    textAckPending_ = false;
    (void)sendTextAck(ackSeq, ackSource, ackHop);
  }

  // Forwarding is deliberately serialized outside the receive critical
  // section. Voice/PTT keeps priority; one queued packet is attempted per task
  // iteration to bound airtime and CPU/RAM impact.
  if (!isPttOrRecording() && !pendingTx_.active && !forwardInFlightActive_ &&
      static_cast<int32_t>(millis() - forwardRetryNotBeforeMs_) >= 0 &&
      millis() - lastForwardTxMs_ >= Config::LORA_FORWARD_RATE_LIMIT_MS) {
    ForwardPacket forward{};
    if (forwardQueue_ && xQueueReceive(forwardQueue_, &forward, 0) == pdPASS) {
      if (transmitForward(forward)) {
        lastForwardTxMs_ = millis();
      } else if (forward.ttl > 0 && forwardQueue_) {
        if (xQueueSend(forwardQueue_, &forward, 0) != pdPASS) {
          ++forwardDrops_;
          forwardLastDropMs_ = millis();
        }
        (void)persistForwardQueue();
      }
    }
  }
}

bool LoRaManager::lbtChannelBusy(int16_t scanStatus) const {
  return scanStatus == RADIOLIB_PREAMBLE_DETECTED ||
         scanStatus == RADIOLIB_LORA_DETECTED;
}

uint8_t LoRaManager::txPriorityForPacket(const String& packet) {
  if (packet.length() < PACKET_HEADER_V2) return 0;
  switch (static_cast<uint8_t>(packet[2])) {
    case Config::LORA_TYPE_SOS: return TX_PRIORITY_SOS;
    case Config::LORA_TYPE_SOS_ACK:
    case Config::LORA_TYPE_TEXT_ACK:
    case Config::LORA_TYPE_VOICE_ACK: return TX_PRIORITY_ACK;
    case Config::LORA_TYPE_VOICE: return TX_PRIORITY_VOICE;
    case Config::LORA_TYPE_TEXT: return TX_PRIORITY_TEXT;
    case Config::LORA_TYPE_NEIGHBOR_BEACON: return TX_PRIORITY_BEACON;
    default: return TX_PRIORITY_FORWARD;
  }
}

bool LoRaManager::queuePendingTx(const String& packet, uint8_t priority) {
  if (!Config::LORA_LBT_ENABLED || packet.isEmpty() || !mutex_) return false;
  if (packet.length() > Config::LORA_MAX_PACKET) return false;
  if (priority == 0) priority = txPriorityForPacket(packet);
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  size_t freeIndex = TX_QUEUE_DEPTH;
  for (size_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
    if (!txQueue_[i].used) { freeIndex = i; break; }
  }
  if (freeIndex == TX_QUEUE_DEPTH) {
    xSemaphoreGive(mutex_);
    return false;
  }
  txQueue_[freeIndex].used = true;
  txQueue_[freeIndex].priority = priority;
  txQueue_[freeIndex].enqueuedMs = millis();
  txQueue_[freeIndex].packet = packet;
  xSemaphoreGive(mutex_);
  return true;
}

bool LoRaManager::processPendingTx() {
  if (!Config::LORA_LBT_ENABLED || !ready_ || !mutex_) return false;
  if (xSemaphoreTake(mutex_, 0) != pdTRUE) return false;
  if (!pendingTx_.active) {
    size_t selected = TX_QUEUE_DEPTH;
    for (size_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
      if (!txQueue_[i].used) continue;
      if (selected == TX_QUEUE_DEPTH ||
          txQueue_[i].priority > txQueue_[selected].priority ||
          (txQueue_[i].priority == txQueue_[selected].priority &&
           static_cast<int32_t>(txQueue_[i].enqueuedMs - txQueue_[selected].enqueuedMs) < 0))
        selected = i;
    }
    if (selected != TX_QUEUE_DEPTH) {
      pendingTx_.packet = txQueue_[selected].packet;
      pendingTx_.retries = 0;
      pendingTx_.nextAttemptMs = millis();
      pendingTx_.active = true;
      txQueue_[selected].packet = String();
      txQueue_[selected].used = false;
    }
  }
  if (!pendingTx_.active ||
      static_cast<int32_t>(millis() - pendingTx_.nextAttemptMs) < 0) {
    xSemaphoreGive(mutex_);
    return false;
  }

  bool done = false;
  bool txOk = false;
  const bool pendingHopped =
      pendingTx_.packet.length() >= PACKET_HEADER_V3 &&
      (static_cast<uint8_t>(pendingTx_.packet[1]) == LORA_PROTOCOL_VERSION_HOP ||
       static_cast<uint8_t>(pendingTx_.packet[1]) == Config::LORA_PROTOCOL_VERSION_ECDH);
  const uint8_t pendingHopIndex = pendingHopped
      ? static_cast<uint8_t>(pendingTx_.packet[14]) : 0;
  if (pendingHopped && !retuneToHopChannelLocked(pendingHopIndex)) {
    pendingTx_.active = false;
    pendingTx_.packet = String();
    done = true;
    {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa V3 pending TX hop retune failed";
    }
  }
  int16_t st = RADIOLIB_ERR_NONE;
  int16_t rxSt = RADIOLIB_ERR_NONE;
  bool budgetConsumed = false;
  RadioLibTime_t airtimeUs = 0;
  String pendingError;

  if (!done) {
    SpiLock spiLock(pdMS_TO_TICKS(20));
    if (spiLock.ok()) {
      st = radio_.scanChannel();
      if (lbtChannelBusy(st)) {
        if (pendingTx_.retries >= Config::LORA_LBT_MAX_RETRIES) {
          pendingTx_.active = false;
          pendingTx_.packet = String();
          done = true;
          rxSt = radio_.startReceive();
          pendingError = "LBT_TIMEOUT";
        } else {
          ++pendingTx_.retries;
          const uint32_t span = Config::LORA_LBT_BACKOFF_MAX_MS -
                                Config::LORA_LBT_BACKOFF_MIN_MS;
          const uint32_t backoff = Config::LORA_LBT_BACKOFF_MIN_MS +
              (span ? (esp_random() % (span + 1U)) : 0U);
          pendingTx_.nextAttemptMs = millis() + backoff;
          // CAD leaves the SX1262 in standby; restore RX while waiting.
          rxSt = radio_.startReceive();
          if (rxSt != RADIOLIB_ERR_NONE) {
            pendingTx_.active = false;
            pendingTx_.packet = String();
            done = true;
            ready_ = false;
            pendingError = "SX1262 RX restart failed after LBT";
          }
        }
      } else if (st == RADIOLIB_CHANNEL_FREE) {
        airtimeUs = radio_.getTimeOnAir(pendingTx_.packet.length());
        if (airtimeUs == 0 || airtimeUs > UINT32_MAX ||
            !consumeDutyBudget(static_cast<uint32_t>(airtimeUs))) {
          pendingTx_.active = false;
          pendingTx_.packet = String();
          done = true;
          st = RADIOLIB_ERR_UNKNOWN;
          rxSt = radio_.startReceive();
          pendingError = "LoRa duty-cycle budget exhausted";
        } else {
          budgetConsumed = true;
          const uint32_t txStartMs = millis();
          st = radio_.transmit(pendingTx_.packet);
          if (millis() - txStartMs > Config::LORA_TX_TIMEOUT_MS)
            st = RADIOLIB_ERR_TX_TIMEOUT;
          rxSt = radio_.startReceive();
          pendingTx_.active = false;
          pendingTx_.packet = String();
          done = true;
          txOk = st == RADIOLIB_ERR_NONE;
          if (txOk) updateAntennaHealthAfterTx();
        }
      } else {
        pendingTx_.active = false;
        pendingTx_.packet = String();
        done = true;
        pendingError = "LBT scan failed: " + String(st);
        rxSt = radio_.startReceive();
      }
    } else {
      st = RADIOLIB_ERR_UNKNOWN;
      pendingTx_.active = false;
      pendingTx_.packet = String();
      done = true;
      pendingError = "LBT SPI lock failed";
    }
  }

  if (!pendingError.isEmpty()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = pendingError;
  }

  if (!txOk && done && forwardInFlightActive_) {
    if (forwardInFlightNextHop_ != 0)
      recordNeighborTxResult(forwardInFlightNextHop_, false);
    const bool retryQueued =
        xQueueSend(forwardQueue_, &forwardInFlight_, pdMS_TO_TICKS(2)) == pdPASS;
    if (!retryQueued) {
      ++forwardDrops_;
      forwardLastDropMs_ = millis();
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa forward retry queue full; packet dropped";
      forwardInFlightNextHop_ = 0;
      forwardRetryPersistId_ = 0;
    } else {
      forwardRetryNotBeforeMs_ = millis() + forwardRetryBackoffMs_;
      forwardRetryBackoffMs_ = min<uint32_t>(5000U, forwardRetryBackoffMs_ * 2U);
    }
    forwardInFlightActive_ = false;
    (void)persistForwardQueue();
  }
  if (txOk && forwardInFlightActive_) {
    if (forwardInFlightNextHop_ != 0)
      recordNeighborTxResult(forwardInFlightNextHop_, true);
    forwardInFlightActive_ = false;
    forwardInFlightNextHop_ = 0;
    forwardRetryPersistId_ = 0;
    forwardRetryBackoffMs_ = 100;
    forwardRetryNotBeforeMs_ = 0;
    (void)persistForwardQueue();
  }

  if (rxSt != RADIOLIB_ERR_NONE) {
    ready_ = false;
    StateLock lock(gState);
    if (lock.ok()) {
      gState.loraReady = false;
      gState.lastError = "SX1262 RX restart failed after LBT";
    }
  } else if (txOk) {
    StateLock lock(gState);
    if (lock.ok()) ++gState.txPackets;
  }
  if (pendingHopped) (void)retuneToChannel0Locked();
  xSemaphoreGive(mutex_);
  return done;
}

bool LoRaManager::transmit(const String& text, bool alreadyEncrypted) {
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  if (!alreadyEncrypted) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "ECDH peer unsupported";
    return false;
  }
#endif
  if (!ready_ || !mutex_ || text.isEmpty() ||
      text.length() > (alreadyEncrypted
          ? Config::LORA_MAX_PACKET
          : Config::LORA_MAX_PACKET - PACKET_HEADER_TX - PACKET_TAG - ROUTE_EXT_BYTES))
    return false;
  if (Config::LORA_REQUIRE_ENCRYPTION && gConfig.loraKeyHex.length() != 32)
    return false;

  String packet;
  uint8_t txType = Config::LORA_TYPE_TEXT;
  uint16_t txSeq = 0;
  uint8_t txTtl = Config::LORA_INITIAL_TTL;
  if (alreadyEncrypted) {
    packet = text;
  } else {
    const uint8_t packetType = Config::LORA_TYPE_TEXT;
    uint16_t seq = 0;
    if (!nextTxSequence(seq)) return false;
    txType = packetType;
    txSeq = seq;
    uint8_t routed[Config::LORA_MAX_PACKET] = {};
    const size_t routedLen = addRouteExtension(
        reinterpret_cast<const uint8_t*>(text.c_str()), text.length(), 0, 0,
        routed, sizeof(routed));
    if (!routedLen ||
        !encryptPacket(routed, routedLen, packetType, seq, packet)) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa encryption/key configuration failed";
      return false;
    }
  }

  if (alreadyEncrypted) {
    if (packet.length() < PACKET_HEADER_V2 ||
        static_cast<uint8_t>(packet[0]) != PACKET_MAGIC)
      return false;
    const uint8_t wireVersion = static_cast<uint8_t>(packet[1]);
    if (wireVersion != Config::LORA_PROTOCOL_VERSION &&
        wireVersion != LORA_PROTOCOL_VERSION_HOP &&
        wireVersion != Config::LORA_PROTOCOL_VERSION_GCM &&
        wireVersion != Config::LORA_PROTOCOL_VERSION_ECDH)
      return false;
    txType = static_cast<uint8_t>(packet[2]);
    txSeq = static_cast<uint16_t>(static_cast<uint8_t>(packet[3])) |
            (static_cast<uint16_t>(static_cast<uint8_t>(packet[4])) << 8);
    txTtl = (wireVersion == Config::LORA_PROTOCOL_VERSION_GCM &&
             packet.length() > 21)
        ? static_cast<uint8_t>(packet[21])
        : static_cast<uint8_t>(packet[13]);
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
    if (wireVersion != Config::LORA_PROTOCOL_VERSION_ECDH &&
        !LoRaEcdhRekey::ecdhBeaconWireAllowed(wireVersion, txType)) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "ECDH peer unsupported";
      return false;
    }
#endif
  }

  if (Config::LORA_LBT_ENABLED) {
    // Serialize the whole pending envelope assignment with the LoRa task;
    // String mutation is not atomic and can otherwise race processPendingTx().
    if (!queuePendingTx(packet)) return false;
    // Try once immediately. If CAD reports a busy channel, task() will
    // continue the retry state machine after a randomized backoff.
    (void)processPendingTx();
    return true;
  }

  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  const bool hoppedPacket =
      packet.length() >= PACKET_HEADER_V3 &&
      (static_cast<uint8_t>(packet[1]) == LORA_PROTOCOL_VERSION_HOP ||
       static_cast<uint8_t>(packet[1]) == Config::LORA_PROTOCOL_VERSION_ECDH);
  const uint8_t packetHopIndex = hoppedPacket
      ? static_cast<uint8_t>(packet[14]) : 0;
  if (hoppedPacket && !retuneToHopChannelLocked(packetHopIndex)) {
    xSemaphoreGive(mutex_);
    return false;
  }
  int16_t st = -1;
  int16_t rxSt = -1;
  {
    SpiLock spiLock(pdMS_TO_TICKS(1000));
    if (!spiLock.ok()) {
      xSemaphoreGive(mutex_);
      return false;
    }

    const RadioLibTime_t airtimeUs = radio_.getTimeOnAir(packet.length());
    if (airtimeUs == 0 || airtimeUs > UINT32_MAX ||
        !consumeDutyBudget(static_cast<uint32_t>(airtimeUs))) {
      xSemaphoreGive(mutex_);
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa duty-cycle budget exhausted";
      return false;
    }

    const uint32_t txStartMs = millis();
    st = radio_.transmit(packet);
    if (st == RADIOLIB_ERR_NONE) {
      updateAntennaHealthAfterTx();
    }
    const uint32_t txElapsedMs = millis() - txStartMs;
    if (txElapsedMs > Config::LORA_TX_TIMEOUT_MS) {
      ready_ = false;
      st = RADIOLIB_ERR_TX_TIMEOUT;
    }
    rxSt = radio_.startReceive();

  }

  bool ok = (st == RADIOLIB_ERR_NONE);
  if (hoppedPacket && !retuneToChannel0Locked()) {
    ok = false;
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "SX1262 channel-0 retune failed after TX";
  }
  {
    StateLock lock(gState);
    if (lock.ok()) {
      if (rxSt != RADIOLIB_ERR_NONE) {
        ready_ = false;
        gState.loraReady = false;
        gState.lastError = "SX1262 RX restart failed: " + String(rxSt);
      }
      if (ok) gState.txPackets++;
    }
  }
  if (ok) logPacket(true, txType, txSeq, sourceId_, 0, 0.0f, txTtl);
  xSemaphoreGive(mutex_);
  return ok;
}

bool LoRaManager::prepareForFactoryReset() {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(50));
  if (!radioGuard.ok()) return false;
  if (!mutex_) return false;
  storageResetting_.store(true, std::memory_order_release);
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    storageResetting_.store(false, std::memory_order_release);
    return false;
  }
  // Also take SPI once after freezing new persistence. If a persistForwardQueue()
  // was already in progress, this waits for its file operation to finish before
  // the caller deletes /LORA.
  {
    SpiLock spiLock(pdMS_TO_TICKS(1000));
    if (!spiLock.ok()) {
      xSemaphoreGive(mutex_);
      storageResetting_.store(false, std::memory_order_release);
      return false;
    }
  }
  xSemaphoreGive(mutex_);
  return true;
}

void LoRaManager::cancelFactoryReset() {
  storageResetting_.store(false, std::memory_order_release);
}

bool LoRaManager::prepareForDeepSleep() {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(50));
  if (!radioGuard.ok()) return false;
  rtcRadioState.magic = RTC_RADIO_MAGIC;
  rtcRadioState.hopFrame = hopFrame_;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  // DECISION: persist only ECDH lifecycle metadata in RTC RAM. Ephemeral
  // private material remains RAM-only and is always regenerated after boot.
  rtcRadioState.ecdhKeyEpoch = ecdhKeyEpoch_;
  rtcRadioState.ecdhActive = ecdhActive_;
#endif
  rtcRadioState.sosSeq = sosSeq_.load(std::memory_order_acquire);
  rtcRadioState.sosRetryCount = sosRetryCount_;
  rtcRadioState.sosAwaitingAck = sosAwaitingAck_;
  rtcRadioState.sosElapsedMs = sosAwaitingAck_ ? millis() - sosSentMs_ : 0;
  rtcRadioState.sosPacketLen = 0;
  memset(rtcRadioState.sosPacket, 0, sizeof(rtcRadioState.sosPacket));
  if (sosAwaitingAck_ && sosPacket_.length() <= Config::LORA_MAX_PACKET) {
    rtcRadioState.sosPacketLen = static_cast<uint16_t>(sosPacket_.length());
    if (rtcRadioState.sosPacketLen)
      memcpy(rtcRadioState.sosPacket, sosPacket_.c_str(), rtcRadioState.sosPacketLen);
  }
  rtcRadioState.crc = stateCrc(rtcRadioState);

  if (!ready_ || !mutex_) return ready_;

  // textStateMutex_ participates in the text->mutex lock order used by
  // transmitHopped()/serviceTextRetry(). Hold it while taking mutex_ so the
  // deep-sleep check cannot race a text transaction or invert the lock order.
  if (!textStateMutex_ ||
      xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    xSemaphoreGive(textStateMutex_);
    return false;
  }

  if (fragmentRxDirty_ && !persistFragmentRx()) {
    xSemaphoreGive(mutex_);
    xSemaphoreGive(textStateMutex_);
    return false;
  }

  bool txQueued = pendingTx_.active || forwardInFlightActive_ ||
                  voiceTxOutstanding_ != 0 || textAwaitingAck_;
  if (!txQueued) {
    for (const auto& entry : txQueue_) {
      if (entry.used) {
        txQueued = true;
        break;
      }
    }
  }
  if (txQueued) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "Deep sleep blocked: TX transaction pending";
    xSemaphoreGive(mutex_);
    xSemaphoreGive(textStateMutex_);
    return false;
  }

  bool armed = false;
  {
    SpiLock spiLock(pdMS_TO_TICKS(1000));
    if (spiLock.ok()) {
      // Drop stale IRQ state before arming duty-cycle RX. A new packet that
      // arrives after this point is intentionally allowed to wake the MCU.
      (void)radio_.clearIrqFlags(0xFFFFU);
      const int16_t st = radio_.startReceiveDutyCycleAuto(
          Config::LORA_PREAMBLE, Config::LORA_RX_DUTY_MIN_SYMBOLS);
      armed = (st == RADIOLIB_ERR_NONE);
      if (armed) {
        ready_ = true;
      } else {
        StateLock lock(gState);
        if (lock.ok()) gState.lastError = "SX1262 duty-cycle RX arm failed";
      }
    } else {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "Deep sleep blocked: SPI busy";
    }
  }

  xSemaphoreGive(mutex_);
  xSemaphoreGive(textStateMutex_);
  return armed;
}

bool LoRaManager::persistMessageHistory() {
  if (!storage.ready()) return false;

  MessageHistoryEntry snapshot[Config::MESSAGE_HISTORY_SIZE] = {};
  size_t count = 0;
  size_t next = 0;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    count = min(gState.messageHistoryCount, Config::MESSAGE_HISTORY_SIZE);
    next = gState.messageHistoryNext;
    const size_t start = (next + Config::MESSAGE_HISTORY_SIZE - count) %
                         Config::MESSAGE_HISTORY_SIZE;
    for (size_t i = 0; i < count; ++i)
      snapshot[i] = gState.messageHistory[(start + i) % Config::MESSAGE_HISTORY_SIZE];
  }

  SpiLock spiLock(pdMS_TO_TICKS(500));
  if (!spiLock.ok()) return false;
  if (!SD.exists("/LOG") && !SD.mkdir("/LOG")) return false;

  const char* path = "/LOG/MSG.LOG";
  File existing = SD.open(path, FILE_READ);
  const uint32_t existingSize = existing ? static_cast<uint32_t>(existing.size()) : 0;
  if (existing) existing.close();
  if (existingSize >= Config::MESSAGE_HISTORY_ROTATE_BYTES) {
    SD.remove("/LOG/MSG.LOG.2");
    if (SD.exists("/LOG/MSG.LOG.1")) SD.rename("/LOG/MSG.LOG.1", "/LOG/MSG.LOG.2");
    if (SD.exists(path)) SD.rename(path, "/LOG/MSG.LOG.1");
  }
  // MSG.LOG is a snapshot of the in-RAM ring, not an append-only event log.
  // Rewriting it prevents every periodic persist from duplicating all messages.
  SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.println("epoch,source,read,text");
  bool ok = true;
  for (size_t i = 0; i < count && ok; ++i) {
    String escaped;
    escaped.reserve(snapshot[i].text.length() + 8);
    for (size_t k = 0; k < snapshot[i].text.length(); ++k) {
      const char c = snapshot[i].text[k];
      if (c == '"') escaped += "\"\"";
      else escaped += c;
    }
    ok = f.printf("%llu,%lu,%u,\"%s\"\n",
                  static_cast<unsigned long long>(snapshot[i].timestamp),
                  static_cast<unsigned long>(snapshot[i].sourceId),
                  snapshot[i].read ? 1U : 0U, escaped.c_str()) > 0;
  }
  f.close();
  return ok;
}

static bool parseMessageCsvLine(const String& line, MessageHistoryEntry& out) {
  const int c1 = line.indexOf(',');
  const int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
  const int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;
  if (c1 <= 0 || c2 <= c1 || c3 <= c2) return false;
  char* end = nullptr;
  const unsigned long long epoch = strtoull(line.substring(0, c1).c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  const unsigned long source = strtoul(line.substring(c1 + 1, c2).c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  const int read = line.substring(c2 + 1, c3).toInt();
  String text = line.substring(c3 + 1);
  if (text.length() >= 2 && text[0] == '"' && text[text.length() - 1] == '"') {
    text = text.substring(1, text.length() - 1);
    String unescaped;
    unescaped.reserve(text.length());
    for (size_t i = 0; i < text.length(); ++i) {
      if (text[i] == '"' && i + 1 < text.length() && text[i + 1] == '"') ++i;
      unescaped += text[i];
    }
    text = unescaped;
  }
  out.timestamp = epoch;
  out.sourceId = static_cast<uint32_t>(source);
  out.read = read != 0;
  out.text = text;
  return true;
}

bool LoRaManager::loadMessageHistory() {
  if (!storage.ready()) return false;
  SpiLock spiLock(pdMS_TO_TICKS(500));
  if (!spiLock.ok()) return false;
  File f = SD.open("/LOG/MSG.LOG", FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }

  MessageHistoryEntry loaded[Config::MESSAGE_HISTORY_SIZE] = {};
  size_t count = 0;
  size_t loadNext = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("epoch,")) continue;
    MessageHistoryEntry e{};
    if (!parseMessageCsvLine(line, e)) continue;
    loaded[loadNext] = e;
    loadNext = (loadNext + 1) % Config::MESSAGE_HISTORY_SIZE;
    if (count < Config::MESSAGE_HISTORY_SIZE) ++count;
  }
  f.close();

  StateLock lock(gState);
  if (!lock.ok()) return false;
  for (auto& e : gState.messageHistory) e = MessageHistoryEntry{};
  gState.messageHistoryNext = 0;
  gState.messageHistoryCount = 0;
  gState.messageUnreadCount = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t loadStart = count == Config::MESSAGE_HISTORY_SIZE ? loadNext : 0;
    const MessageHistoryEntry& e =
        loaded[(loadStart + i) % Config::MESSAGE_HISTORY_SIZE];
    gState.messageHistory[gState.messageHistoryNext] = e;
    if (!e.read) ++gState.messageUnreadCount;
    gState.messageHistoryNext =
        (gState.messageHistoryNext + 1) % Config::MESSAGE_HISTORY_SIZE;
    ++gState.messageHistoryCount;
    gState.lastMessage = e.text;
  }
  return true;
}

String LoRaManager::neighborsJson() const {
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return "[]";
  String out = "[";
  const uint32_t now = millis();
  bool first = true;
  for (const auto& n : neighbors_) {
    if (!n.sourceId || n.seenMs == 0) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"sourceId\":" + String(n.sourceId) +
           ",\"rssi\":" + String(n.rssi) +
           ",\"snr\":" + String(n.snr, 1) +
           ",\"quality\":" + String(n.quality) +
           ",\"txAttempts\":" + String(n.txAttempts) +
           ",\"txSuccess\":" + String(n.txSuccess) +
           ",\"ageMs\":" + String(now - n.seenMs) + "}";
  }
  out += "]";
  if (mutex_) xSemaphoreGive(mutex_);
  return out;
}

String LoRaManager::routesJson() const {
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return "[]";
  String out = "[";
  const uint32_t now = millis();
  bool first = true;
  for (const auto& r : routes_) {
    if (!r.destination || !r.nextHop || r.seenMs == 0) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"destination\":" + String(r.destination) +
           ",\"nextHop\":" + String(r.nextHop) +
           ",\"etxQ8\":" + String(r.etxQ8) +
           ",\"quality\":" + String(r.quality) +
           ",\"ageMs\":" + String(now - r.seenMs) + "}";
  }
  out += "]";
  if (mutex_) xSemaphoreGive(mutex_);
  return out;
}

bool LoRaManager::capturePacket(const String& raw, int16_t rssi, float snr, bool decrypted) {
  if (!captureMutex_ || !captureActive()) return false;
  if (xSemaphoreTake(captureMutex_, pdMS_TO_TICKS(5)) != pdTRUE) return false;
  if (!captureUntilMs_ || static_cast<int32_t>(millis() - captureUntilMs_) >= 0) {
    xSemaphoreGive(captureMutex_);
    return false;
  }
  CaptureEntry& e = capture_[captureNext_];
  e.ts = millis();
  e.rssi = rssi;
  e.snr = snr;
  e.decrypted = decrypted;
  e.rawHex.reserve(raw.length() * 2);
  e.rawHex = "";
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < raw.length(); ++i) {
    const uint8_t b = static_cast<uint8_t>(raw[i]);
    e.rawHex += hex[b >> 4];
    e.rawHex += hex[b & 0x0F];
  }
  captureNext_ = (captureNext_ + 1) % CAPTURE_SIZE;
  if (captureCount_ < CAPTURE_SIZE) ++captureCount_;
  xSemaphoreGive(captureMutex_);
  return true;
}

bool LoRaManager::captureStart(uint32_t durationMs) {
  if (!captureMutex_ || durationMs == 0 ||
      durationMs > Config::CAPTURE_MAX_DURATION_MS) return false;
  if (xSemaphoreTake(captureMutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  captureNext_ = 0;
  captureCount_ = 0;
  captureUntilMs_ = millis() + durationMs;
  for (auto& e : capture_) e = CaptureEntry{};
  xSemaphoreGive(captureMutex_);
  return true;
}

bool LoRaManager::captureStop() {
  if (!captureMutex_ || xSemaphoreTake(captureMutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;
  captureUntilMs_ = 0;
  xSemaphoreGive(captureMutex_);
  return true;
}

bool LoRaManager::captureActive() const {
  return captureUntilMs_ != 0 &&
         static_cast<int32_t>(millis() - captureUntilMs_) < 0;
}

String LoRaManager::captureDumpJson() const {
  if (!captureMutex_ || xSemaphoreTake(captureMutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return "[]";
  String out = "[";
  const size_t count = captureCount_;
  const size_t start = (captureNext_ + CAPTURE_SIZE - count) % CAPTURE_SIZE;
  for (size_t i = 0; i < count; ++i) {
    if (i) out += ",";
    const auto& e = capture_[(start + i) % CAPTURE_SIZE];
    out += "{\"ts\":" + String(e.ts) +
           ",\"rssi\":" + String(e.rssi) +
           ",\"snr\":" + String(e.snr, 1) +
           ",\"rawHex\":\"" + e.rawHex +
           "\",\"decrypted\":" + String(e.decrypted ? "true" : "false") + "}";
  }
  out += "]";
  xSemaphoreGive(captureMutex_);
  return out;
}

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
String LoRaManager::ecdhStatusJson() const {
  size_t peerCount = 0;
  for (const auto& peer : ecdhPeers_) {
    if (peer.valid) ++peerCount;
  }
  const uint32_t nowEpochSec = currentEpochSec();
  const uint32_t nowEpoch = nowEpochSec != 0
      ? LoRaEcdhRekey::epochNumber(nowEpochSec) : 0;
  return String("{\"enabled\":true,\"active\":") +
         (ecdhActive_ ? "true" : "false") +
         ",\"keyEpoch\":" + String(ecdhKeyEpoch_) +
         ",\"runtimeEpoch\":" + String(nowEpoch) +
         ",\"authoritativeEpoch\":" +
         String(ecdhAuthoritativeEpochSec_ != 0
                    ? LoRaEcdhRekey::epochNumber(ecdhAuthoritativeEpochSec_)
                    : 0) +
         ",\"ephemeralEpoch\":" +
         String(ecdhKeyMaterial_.ephemeralEpoch()) +
         ",\"peerCount\":" + String(peerCount) + "}";
}
#else
String LoRaManager::ecdhStatusJson() const {
  return "{\"enabled\":false}";
}
#endif

bool LoRaManager::setAdrEnabled(bool enabled) {
  adrEnabled_ = enabled;
  if (!enabled) {
    currentAdrSf_ = gConfig.loraSf;
    return true;
  }
  serviceAdr();
  return true;
}

void LoRaManager::serviceAdr() {
  if (!adrEnabled_ || !ready_) return;
  const uint8_t quality = bestNeighborQuality();
  const uint8_t target = quality > 70 ? 7 : (quality >= 40 ? 9 : 11);
  if (target == currentAdrSf_) return;
  if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return;
  const uint8_t oldSf = gConfig.loraSf;
  gConfig.loraSf = target;
  currentAdrSf_ = target;
  bool ok = false;
  {
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (spiLock.ok()) {
      const int16_t st = radio_.begin(
          gConfig.loraFreqMHz, gConfig.loraBwKHz, target,
          gConfig.loraCr, gConfig.loraSyncWord, gConfig.loraPowerDbm,
          Config::LORA_PREAMBLE, Config::LORA_TCXO_VOLTAGE);
      if (st == RADIOLIB_ERR_NONE) {
        radio_.setPacketReceivedAction(onDio1);
        ok = radio_.startReceive() == RADIOLIB_ERR_NONE;
      }
    }
  }
  if (!ok) {
    gConfig.loraSf = oldSf;
    currentAdrSf_ = oldSf;
  }
  xSemaphoreGive(mutex_);
}

uint8_t LoRaManager::lqi() const {
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  int best = 0;
  for (const auto& n : neighbors_) {
    if (!n.sourceId || n.seenMs == 0) continue;
    const float rssiScore = constrain((static_cast<float>(n.rssi) + 120.0f) * 2.0f, 0.0f, 100.0f);
    const float snrScore = constrain((n.snr + 20.0f) * 2.5f, 0.0f, 100.0f);
    const float perScore = n.txAttempts
        ? (100.0f * static_cast<float>(n.txSuccess) / n.txAttempts)
        : 100.0f;
    const int score = static_cast<int>(0.4f * rssiScore + 0.3f * snrScore + 0.3f * perScore);
    best = max(best, score);
  }
  const uint8_t result = static_cast<uint8_t>(constrain(best, 0, 100));
  if (mutex_) xSemaphoreGive(mutex_);
  return result;
}

bool LoRaManager::setHopSyncSource(bool gps) {
  hopSyncGps_ = gps;
  return true;
}

bool LoRaManager::setSosFormats(uint8_t mask) {
  if ((mask & 0x07U) == 0) return false;
  sosFormatMask_ = mask & 0x07U;
  return true;
}

bool LoRaManager::loadScheduledMessages() {
  Preferences prefs;
  if (!prefs.begin("fieldradio", true)) return false;

  ScheduledStore best{};
  bool have = false;
  const char* keys[] = {"sched_a", "sched_b"};
  for (const char* key : keys) {
    ScheduledStore candidate{};
    if (prefs.getBytes(key, &candidate, sizeof(candidate)) != sizeof(candidate) ||
        candidate.magic != SCHEDULED_STORE_MAGIC ||
        candidate.version != SCHEDULED_STORE_VERSION ||
        candidate.count > SCHEDULED_MESSAGE_MAX)
      continue;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&candidate);
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < offsetof(ScheduledStore, crc); ++i) {
      crc ^= bytes[i];
      for (uint8_t b = 0; b < 8; ++b)
        crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    }
    if (candidate.crc != ~crc) continue;
    if (!have || static_cast<int32_t>(candidate.generation - best.generation) > 0) {
      best = candidate;
      have = true;
    }
  }
  prefs.end();

  if (!have) return true;
  memset(scheduledMessages_, 0, sizeof(scheduledMessages_));
  for (size_t i = 0; i < SCHEDULED_MESSAGE_MAX; ++i) {
    const ScheduledStoreEntry& src = best.entries[i];
    if (!src.used || !src.id || !src.atEpoch || src.text[0] == '\0') continue;
    scheduledMessages_[i].used = true;
    scheduledMessages_[i].id = src.id;
    scheduledMessages_[i].atEpoch = src.atEpoch;
    scheduledMessages_[i].text = String(src.text);
  }
  nextScheduledMessageId_ = best.nextId ? best.nextId : 1;
  scheduledStoreGeneration_ = best.generation;
  return true;
}

bool LoRaManager::persistScheduledMessages() {
  ScheduledStore store{};
  store.magic = SCHEDULED_STORE_MAGIC;
  store.version = SCHEDULED_STORE_VERSION;
  store.count = 0;
  store.generation = scheduledStoreGeneration_ + 1U;
  if (store.generation == 0) store.generation = 1;
  store.nextId = nextScheduledMessageId_;
  for (size_t i = 0; i < SCHEDULED_MESSAGE_MAX; ++i) {
    const ScheduledMessage& src = scheduledMessages_[i];
    ScheduledStoreEntry& dst = store.entries[i];
    if (!src.used || !src.id || !src.atEpoch || src.text.isEmpty() ||
        src.text.length() >= sizeof(dst.text))
      continue;
    dst.used = 1;
    dst.id = src.id;
    dst.atEpoch = src.atEpoch;
    src.text.toCharArray(dst.text, sizeof(dst.text));
    ++store.count;
  }
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&store);
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < offsetof(ScheduledStore, crc); ++i) {
    crc ^= bytes[i];
    for (uint8_t b = 0; b < 8; ++b)
      crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
  }
  store.crc = ~crc;

  const char* key = (store.generation & 1U) ? "sched_a" : "sched_b";
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) return false;
  const bool ok = prefs.putBytes(key, &store, sizeof(store)) == sizeof(store);
  prefs.end();
  if (!ok) return false;

  // Read-back verification makes the bank switch transactional from the
  // application's perspective; the previous bank remains recoverable.
  Preferences verifyPrefs;
  if (!verifyPrefs.begin("fieldradio", true)) return false;
  ScheduledStore verify{};
  const bool verified = verifyPrefs.getBytes(key, &verify, sizeof(verify)) == sizeof(verify) &&
                        verify.magic == SCHEDULED_STORE_MAGIC &&
                        verify.version == SCHEDULED_STORE_VERSION &&
                        verify.generation == store.generation &&
                        verify.crc == store.crc;
  verifyPrefs.end();
  if (verified) scheduledStoreGeneration_ = store.generation;
  return verified;
}

bool LoRaManager::scheduleMessage(uint64_t atEpoch, const String& text) {
  if (!atEpoch || text.isEmpty() ||
      text.length() > Config::LORA_MAX_PACKET - PACKET_HEADER_V3 -
                       PACKET_TAG - ROUTE_EXT_BYTES || !mutex_)
    return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  for (auto& entry : scheduledMessages_) {
    if (!entry.used) {
      entry.used = true;
      entry.id = nextScheduledMessageId_++;
      if (nextScheduledMessageId_ == 0) nextScheduledMessageId_ = 1;
      entry.atEpoch = atEpoch;
      entry.text = text;
      const bool persisted = persistScheduledMessages();
      if (!persisted) entry = ScheduledMessage{};
      xSemaphoreGive(mutex_);
      return persisted;
    }
  }
  xSemaphoreGive(mutex_);
  return false;
}

bool LoRaManager::cancelScheduledMessage(uint32_t id) {
  if (!id || !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  for (auto& entry : scheduledMessages_) {
    if (entry.used && entry.id == id) {
      entry = ScheduledMessage{};
      const bool persisted = persistScheduledMessages();
      xSemaphoreGive(mutex_);
      return persisted;
    }
  }
  xSemaphoreGive(mutex_);
  return false;
}

String LoRaManager::scheduledMessagesJson() const {
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return "[]";
  String out = "[";
  bool first = true;
  for (const auto& e : scheduledMessages_) {
    if (!e.used) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"id\":" + String(e.id) +
           ",\"at\":" + String(static_cast<unsigned long long>(e.atEpoch)) +
           ",\"text\":\"" + e.text + "\"}";
  }
  out += "]";
  if (mutex_) xSemaphoreGive(mutex_);
  return out;
}

void LoRaManager::serviceScheduledMessages() {
  if (!mutex_ || !ready_) return;
  const uint32_t nowMs = millis();
  static uint32_t lastRunMs = 0;
  if (nowMs - lastRunMs < 250) return;
  lastRunMs = nowMs;
  uint64_t nowEpoch = 0;
  {
    StateLock lock(gState);
    if (lock.ok() && gState.gps.timeValid) nowEpoch = gState.gps.utcEpoch;
  }
  if (!nowEpoch) return;

  String text;
  uint32_t id = 0;
  if (xSemaphoreTake(mutex_, 0) != pdTRUE) return;
  for (auto& e : scheduledMessages_) {
    if (e.used && e.atEpoch <= nowEpoch) {
      id = e.id;
      text = e.text;
      e = ScheduledMessage{};
      if (!persistScheduledMessages()) {
        // Do not lose a scheduled item merely because persistence failed.
        // Restore it; transmission is deferred until the next service pass.
        e.used = true;
        e.id = id;
        e.atEpoch = nowEpoch;
        e.text = text;
        id = 0;
        text.clear();
      }
      break;
    }
  }
  xSemaphoreGive(mutex_);
  if (id && !text.isEmpty()) (void)transmitHopped(text, Config::LORA_TYPE_TEXT, 0);
}


void LoRaManager::updateSourceId() {
  const uint32_t next = sourceIdFromCallsign(gConfig.callsign);
  if (next == 0) return;
  if (textStateMutex_ &&
      xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    // A callsign change changes the authenticated identity. Do not allow an
    // ACK for the previous identity to complete the new text transaction.
    textAwaitingAck_ = false;
    textAcked_ = false;
    textPendingPacket_.clear();
    sourceId_ = next;
    xSemaphoreGive(textStateMutex_);
  } else {
    // Never mutate sourceId_ outside the text-state transaction. An unlocked
    // update could race packet construction and bind an ACK to a new identity.
    return;
  }
}

bool LoRaManager::applyConfig() {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(50));
  if (!radioGuard.ok()) return false;
  if (!mutex_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

  bool ok = false;
  {
    SpiLock spiLock(pdMS_TO_TICKS(1000));
    if (spiLock.ok()) {
      const int16_t st = radio_.begin(
          gConfig.loraFreqMHz, gConfig.loraBwKHz, gConfig.loraSf,
          gConfig.loraCr, gConfig.loraSyncWord, gConfig.loraPowerDbm,
          Config::LORA_PREAMBLE, Config::LORA_TCXO_VOLTAGE);
      if (st == RADIOLIB_ERR_NONE) {
        radio_.setPacketReceivedAction(onDio1);
        ok = radio_.startReceive() == RADIOLIB_ERR_NONE;
      }
    }
  }

  ready_ = ok;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.loraReady = ok;
      if (!ok) gState.lastError = "LoRa reconfiguration failed";
    }
  }
  xSemaphoreGive(mutex_);
  return ok;
}

bool LoRaManager::sendText(const String& text) {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(20));
  if (!radioGuard.ok()) return false;
  return sendTextTo(0, text);
}

bool LoRaManager::sendTextTo(uint32_t destination, const String& text) {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(20));
  if (!radioGuard.ok()) return false;
  if (text.isEmpty()) return false;
  if (!textStateMutex_ ||
      xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(50)) != pdTRUE)
    return false;
  const bool self = destination == sourceId_;
  xSemaphoreGive(textStateMutex_);
  if (self) return false;
  if (text.length() > Config::LORA_MAX_PACKET - PACKET_HEADER_V3 - PACKET_TAG - ROUTE_EXT_BYTES)
    return enqueueTextFragments(text, destination);
  // Transmission is synchronous only for the radio airtime/LBT operation.
  // ACK/retry processing is owned by serviceTextRetry() in the LoRa task.
  return transmitHopped(text, Config::LORA_TYPE_TEXT, destination);
}

bool LoRaManager::sendVoiceFrame() {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(20));
  if (!radioGuard.ok()) return false;
  if (!ready_ || voiceTxOutstanding_ >= Config::LORA_VOICE_WINDOW_SIZE)
    return false;

  VoiceTxSlot* slot = nullptr;
  for (auto& candidate : voiceTx_)
    if (!candidate.used) { slot = &candidate; break; }
  if (!slot) return false;

  uint8_t captured[164] = {};
  uint8_t frame[168] = {};
  size_t len = 0;
  if (!audio.captureVoiceFrame(captured, sizeof(captured), len) || len != sizeof(captured))
    return false;

  memcpy(frame, captured, 4);
  const uint16_t seq = voiceSequence_++;
  frame[4] = static_cast<uint8_t>(seq & 0xFF);
  frame[5] = static_cast<uint8_t>(seq >> 8);
  memcpy(frame + 6, captured + 4, 160);
  const uint16_t crc = crc16(frame, 166);
  frame[166] = static_cast<uint8_t>(crc & 0xFF);
  frame[167] = static_cast<uint8_t>(crc >> 8);

  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const size_t routedLen = addRouteExtension(frame, sizeof(frame), 0, 0,
                                              routed, sizeof(routed));
  String packet;
  if (!routedLen ||
      !encryptPacket(routed, routedLen, Config::LORA_TYPE_VOICE, seq, packet) ||
      packet.length() > Config::LORA_MAX_PACKET || !transmit(packet, true))
    return false;

  slot->used = true;
  slot->acked = false;
  slot->seq = seq;
  slot->packet = packet;
  slot->retries = 0;
  slot->sentMs = millis();
  slot->nextAttemptMs = slot->sentMs + Config::LORA_VOICE_ACK_TIMEOUT_MS;
  slot->peerSourceId = 0;
  // Seed retry timing from the freshest neighbor metric. A zero-quality value
  // would otherwise pessimistically force the first retry onto the weak-peer
  // timeout even when a strong neighbor is already known.
  slot->peerQuality = bestNeighborQuality();
  ++voiceTxOutstanding_;

  {
    StateLock lock(gState);
    if (lock.ok()) ++gState.voiceTxPackets;
  }
  logPacket(true, Config::LORA_TYPE_VOICE, seq, sourceId_, 0, 0.0f,
            Config::LORA_INITIAL_TTL);
  return true;
}

bool LoRaManager::sendVoiceAck(uint16_t ackedSeq, uint32_t ackedSourceId,
                               int16_t rssi, float snr) {
  uint8_t payload[VOICE_ACK_BYTES] = {};
  payload[0] = VOICE_ACK_MAGIC;
  payload[1] = VOICE_ACK_VERSION;
  memcpy(payload + 2, &ackedSourceId, 4);
  memcpy(payload + 6, &voiceRxAckBase_, 2);
  memcpy(payload + 8, &voiceRxAckBitmap_, 1);
  memcpy(payload + 10, &rssi, 2);
  const int16_t snr10 = static_cast<int16_t>(constrain(
      static_cast<int32_t>(lroundf(snr * 10.0f)), -32768, 32767));
  memcpy(payload + 12, &snr10, 2);
  payload[14] = Config::LORA_VOICE_WINDOW_SIZE;
  payload[15] = static_cast<uint8_t>(ackedSeq == voiceRxAckBase_ ? 1U : 0U);

  uint16_t seq = 0;
  if (!nextTxSequence(seq)) return false;
  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const size_t routedLen = addRouteExtension(
      payload, sizeof(payload), ackedSourceId, 0, routed, sizeof(routed));
  String packet;
  if (!routedLen ||
      !encryptPacket(routed, routedLen, Config::LORA_TYPE_VOICE_ACK, seq, packet))
    return false;
  return transmit(packet, true);
}

void LoRaManager::updateNeighborMetric(uint32_t sourceId, int16_t rssi, float snr) {
  if (sourceId == 0 || sourceId == sourceId_) return;
  size_t selected = NEIGHBOR_CACHE_SIZE;
  for (size_t i = 0; i < NEIGHBOR_CACHE_SIZE; ++i) {
    if (neighbors_[i].sourceId == sourceId) { selected = i; break; }
    if (selected == NEIGHBOR_CACHE_SIZE && neighbors_[i].seenMs == 0) selected = i;
  }
  if (selected == NEIGHBOR_CACHE_SIZE) {
    selected = neighborNext_;
    neighborNext_ = (neighborNext_ + 1) % NEIGHBOR_CACHE_SIZE;
  }
  auto& entry = neighbors_[selected];
  if (entry.sourceId != sourceId || entry.seenMs == 0) {
    entry.sourceId = sourceId;
    entry.rssi = rssi;
    entry.snr = snr;
    entry.txAttempts = 0;
    entry.txSuccess = 0;
  } else {
    entry.rssi = static_cast<int16_t>((static_cast<int32_t>(entry.rssi) * 3 + rssi) / 4);
    entry.snr = entry.snr * 0.75f + snr * 0.25f;
  }
  entry.seenMs = millis();
  const int score = constrain((entry.rssi + 120) * 2 +
                              static_cast<int>(entry.snr * 3.0f) + 60, 0, 100);
  entry.quality = static_cast<uint8_t>(score);
}

uint8_t LoRaManager::neighborQualityForPeer(uint32_t sourceId) const {
  if (sourceId == 0) return 0;
  const uint32_t now = millis();
  for (const auto& entry : neighbors_) {
    if (entry.sourceId != sourceId || entry.seenMs == 0) continue;
    const uint32_t age = now - entry.seenMs;
    if (age > Config::NEIGHBOR_TTL_MS) return 0;
    // Keep a stale-but-recent neighbor usable while degrading its quality
    // smoothly instead of dropping it to zero as soon as one beacon is missed.
    const uint8_t decay = static_cast<uint8_t>(
        100U - min<uint32_t>(100U,
            (age * 100U) / max<uint32_t>(1U, Config::NEIGHBOR_TTL_MS)));
    return static_cast<uint8_t>((static_cast<uint16_t>(entry.quality) * decay) / 100U);
  }
  return 0;
}

uint8_t LoRaManager::bestNeighborQuality() const {
  uint8_t best = 0;
  const uint32_t now = millis();
  for (const auto& entry : neighbors_) {
    if (entry.sourceId != 0 && now - entry.seenMs <= Config::NEIGHBOR_TTL_MS)
      best = max(best, entry.quality);
  }
  return best;
}

void LoRaManager::handleVoiceAckPayload(uint32_t ackSenderSourceId,
                                        const uint8_t* payload, size_t len) {
  if (!payload) return;

  uint16_t ackBase = 0;
  uint8_t ackBitmap = 0;
  uint32_t ackedSource = 0;
  int16_t rssi = -127;
  int16_t snr10 = -200;

  if (len == VOICE_ACK_BYTES && payload[0] == VOICE_ACK_MAGIC &&
      payload[1] == VOICE_ACK_VERSION) {
    memcpy(&ackedSource, payload + 2, 4);
    memcpy(&ackBase, payload + 6, 2);
    memcpy(&ackBitmap, payload + 8, 1);
    memcpy(&rssi, payload + 10, 2);
    memcpy(&snr10, payload + 12, 2);
  } else if (len == VOICE_ACK_LEGACY_BYTES) {
    // Accept the Rev-B Level-3 single-frame ACK for mixed-firmware migration.
    uint16_t legacySeq = 0;
    memcpy(&legacySeq, payload, 2);
    memcpy(&ackedSource, payload + 2, 4);
    memcpy(&rssi, payload + 6, 2);
    memcpy(&snr10, payload + 8, 2);
    ackBase = legacySeq;
    ackBitmap = 0;
  } else {
    return;
  }

  if (ackedSource != sourceId_) return;
  updateNeighborMetric(ackSenderSourceId, rssi, static_cast<float>(snr10) / 10.0f);
  const uint8_t quality = neighborQualityForPeer(ackSenderSourceId);

  for (auto& slot : voiceTx_) {
    if (!slot.used || slot.acked) continue;
    // The sender has at most LORA_VOICE_WINDOW_SIZE outstanding frames, so a
    // cumulative ACK is only authoritative within that bounded window. This
    // avoids treating an arbitrarily old sequence as ACKed after uint16 wrap.
    const int16_t delta = static_cast<int16_t>(
        static_cast<uint16_t>(slot.seq - ackBase));
    // Unit-test boundary intent: delta=-WINDOW and +WINDOW are accepted;
    // values outside the bounded signed window are never ACK-authoritative.
    if (delta < -static_cast<int16_t>(Config::LORA_VOICE_WINDOW_SIZE) ||
        delta > static_cast<int16_t>(Config::LORA_VOICE_WINDOW_SIZE))
      continue;
    const int16_t behind = static_cast<int16_t>(-delta);
    bool acked = behind >= 1 &&
                 behind <= static_cast<int16_t>(Config::LORA_VOICE_WINDOW_SIZE);
    if (!acked) {
      const int16_t ahead = delta;
      acked = ahead >= 1 &&
              ahead <= static_cast<int16_t>(Config::LORA_VOICE_WINDOW_SIZE) &&
              (ackBitmap & (1U << static_cast<uint8_t>(ahead - 1)));
    }
    if (acked) {
      slot.acked = true;
      slot.peerSourceId = ackSenderSourceId;
      slot.peerQuality = quality;
    }
  }

  for (auto& slot : voiceTx_) {
    if (slot.used && slot.acked) {
      slot.used = false;
      slot.acked = false;
      slot.packet = String();
      if (voiceTxOutstanding_ > 0) --voiceTxOutstanding_;
    }
  }
}

void LoRaManager::serviceVoiceAckRetry() {
  const uint32_t now = millis();
  for (auto& slot : voiceTx_) {
    if (!slot.used || slot.acked || slot.packet.isEmpty() ||
        static_cast<int32_t>(now - slot.nextAttemptMs) < 0) continue;

    const uint8_t knownQuality = slot.peerQuality ? slot.peerQuality :
                                  (slot.peerSourceId ? neighborQualityForPeer(slot.peerSourceId)
                                                      : bestNeighborQuality());
    if (slot.retries >= Config::LORA_VOICE_MAX_RETRIES) {
      slot.used = false;
      slot.packet = String();
      if (voiceTxOutstanding_ > 0) --voiceTxOutstanding_;
      StateLock lock(gState);
      if (lock.ok()) ++gState.voiceDrops;
      continue;
    }

    const uint32_t timeout = knownQuality < 35
        ? Config::LORA_VOICE_ACK_WEAK_TIMEOUT_MS
        : Config::LORA_VOICE_ACK_TIMEOUT_MS;
    if (now - slot.sentMs < timeout) {
      slot.nextAttemptMs = slot.sentMs + timeout;
      continue;
    }

    if (transmit(slot.packet, true)) {
      ++slot.retries;
      slot.sentMs = millis();
      const uint32_t backoff = knownQuality < 35
          ? Config::LORA_VOICE_ACK_WEAK_TIMEOUT_MS
          : Config::LORA_VOICE_ACK_TIMEOUT_MS;
      slot.nextAttemptMs = slot.sentMs + backoff;
    } else {
      slot.nextAttemptMs = now + Config::LORA_VOICE_ACK_TIMEOUT_MS;
    }
  }
}

void LoRaManager::serviceNeighborBeacon() {
  const uint32_t now = millis();
  for (auto& route : routes_) {
    if (route.seenMs != 0 && now - route.seenMs > ROUTE_CACHE_TTL_MS)
      route = RouteEntry{};
  }
  for (auto& neighbor : neighbors_) {
    if (neighbor.seenMs != 0 && now - neighbor.seenMs > Config::NEIGHBOR_TTL_MS)
      neighbor = NeighborEntry{};
  }
  if (!ready_ || now - lastNeighborBeaconMs_ < Config::LORA_NEIGHBOR_BEACON_PERIOD_MS)
    return;
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  const uint32_t epochSec = currentEpochSec();
  if (epochSec == 0 || !ecdhKeyMaterial_.hasEphemeralKey())
    return;

  LoRaEcdhRekey::Beacon ecdhBeacon{};
  ecdhBeacon.epochSec = epochSec;
  memcpy(ecdhBeacon.ephemeralPublic, ecdhKeyMaterial_.ephemeralPublic(),
         LoRaEcdhRekey::PUBLIC_KEY_BYTES);
  memcpy(ecdhBeacon.staticPublic, ecdhKeyMaterial_.longTermPublic(),
         LoRaEcdhRekey::PUBLIC_KEY_BYTES);
  ecdhBeacon.valid = true;

  uint8_t payload[LoRaEcdhRekey::BEACON_BYTES] = {};
  if (LoRaEcdhRekey::encodeBeacon(
          ecdhBeacon, payload, sizeof(payload)) !=
      LoRaEcdhRekey::BEACON_BYTES)
    return;

  uint16_t seq = 0;
  if (!nextTxSequence(seq)) return;
  String packet;
  // DECISION: carry the ECDH envelope inside authenticated V3 so the
  // existing master-key trust anchor binds sourceId to the static key.
  if (encryptPacketV3(payload, sizeof(payload),
                      Config::LORA_TYPE_NEIGHBOR_BEACON, seq,
                      computeHopIndex(hopFrame_), epochSec, packet)) {
    if (queuePendingTx(packet, TX_PRIORITY_BEACON)) lastNeighborBeaconMs_ = now;
  }
#else
  uint8_t payload[8] = {};
  payload[0] = BEACON_MAGIC;
  payload[1] = Config::LORA_PROTOCOL_VERSION;
  memcpy(payload + 2, &sourceId_, 4);
  uint16_t uptime10 = static_cast<uint16_t>(min<uint32_t>(65535U, now / 1000U));
  memcpy(payload + 6, &uptime10, 2);
  uint16_t seq = 0;
  if (!nextTxSequence(seq)) return;
  String packet;
  if (encryptPacket(payload, sizeof(payload), Config::LORA_TYPE_NEIGHBOR_BEACON, seq, packet)) {
    if (queuePendingTx(packet, TX_PRIORITY_BEACON)) lastNeighborBeaconMs_ = now;
  }
#endif
}

bool LoRaManager::handleTextFragment(uint32_t sourceId, const uint8_t* payload, size_t len) {
  if (!payload || len < Config::LORA_FRAGMENT_HEADER_BYTES || payload[0] != FRAGMENT_MAGIC ||
      payload[1] != Config::LORA_FRAGMENT_VERSION) return false;
  const uint16_t messageId = static_cast<uint16_t>(payload[2]) | (static_cast<uint16_t>(payload[3]) << 8);
  const uint8_t index = payload[4];
  const uint8_t count = payload[5];
  const uint16_t totalLen = static_cast<uint16_t>(payload[6]) | (static_cast<uint16_t>(payload[7]) << 8);
  if (count == 0 || count > Config::LORA_FRAGMENT_MAX_COUNT || index >= count ||
      totalLen == 0 || totalLen > Config::LORA_FRAGMENT_MAX_BYTES) return false;
  if (Config::LORA_MAX_PACKET <= PACKET_HEADER_TX + PACKET_TAG +
                                  Config::LORA_FRAGMENT_HEADER_BYTES + ROUTE_EXT_BYTES)
    return false;
  const size_t chunkMax = Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG -
                          Config::LORA_FRAGMENT_HEADER_BYTES - ROUTE_EXT_BYTES;
  const size_t chunkLen = len - Config::LORA_FRAGMENT_HEADER_BYTES;
  if (chunkMax == 0 || chunkMax > Config::LORA_MAX_PACKET || chunkLen == 0 ||
      chunkLen > chunkMax) return false;
  const size_t offset = static_cast<size_t>(index) * chunkMax;
  if (offset >= totalLen || chunkLen > totalLen - offset) return false;
  const uint32_t now = millis();
  FragmentRxState* state = nullptr;
  FragmentRxState* reusable = nullptr;
  FragmentRxState* expired = nullptr;
  // Locate an existing transaction before considering eviction. A duplicate
  // fragment must never reset an active reassembly.
  for (auto& candidate : fragmentRx_) {
    if (candidate.active && candidate.sourceId == sourceId &&
        candidate.messageId == messageId) {
      state = &candidate;
      break;
    }
  }
  if (!state) {
    for (auto& candidate : fragmentRx_) {
      if (candidate.active &&
          now - candidate.startedMs > Config::LORA_FRAGMENT_REASSEMBLY_TIMEOUT_MS) {
        if (!expired ||
            static_cast<int32_t>(candidate.startedMs - expired->startedMs) < 0)
          expired = &candidate;
      } else if (!candidate.active && !reusable) {
        reusable = &candidate;
      }
    }
    state = expired ? expired : reusable;
    if (expired) {
      ++fragmentEvictions_;
      *expired = FragmentRxState{};
    }
    if (!state) {
      ++fragmentDrops_;
      // Never evict another sender's in-flight message.
      return false;
    }
    if (state == reusable) *state = FragmentRxState{};
  }
    state->active = true;
    state->sourceId = sourceId;
    state->messageId = messageId;
    state->count = count;
    state->totalLen = totalLen;
    state->startedMs = now;
    fragmentRxDirty_ = true;
  } else if (state->count != count || state->totalLen != totalLen) {
    // Same (source,messageId) with conflicting authenticated metadata is not a
    // continuation of the current message; reject it instead of resetting a
    // valid in-flight reassembly.
    ++fragmentDrops_;
    return false;
  }

  const uint16_t bit = static_cast<uint16_t>(1U << index);
  if (!(state->receivedMask & bit)) {
    memcpy(state->data + offset, payload + Config::LORA_FRAGMENT_HEADER_BYTES, chunkLen);
    state->lengths[index] = static_cast<uint16_t>(chunkLen);
    state->receivedMask |= bit;
    state->receivedBytes = static_cast<uint16_t>(state->receivedBytes + chunkLen);
    fragmentRxDirty_ = true;
  } else if (state->lengths[index] != chunkLen ||
             memcmp(state->data + offset,
                    payload + Config::LORA_FRAGMENT_HEADER_BYTES, chunkLen) != 0) {
    // A duplicate fragment must be byte-identical. This catches conflicting
    // fragment variants without destroying the already authenticated state.
    ++fragmentDrops_;
    return false;
  }
  const uint16_t completeMask = static_cast<uint16_t>((1UL << count) - 1UL);
  if (state->receivedMask != completeMask || state->receivedBytes != totalLen) return true;
  char text[Config::LORA_FRAGMENT_MAX_BYTES + 1] = {};
  memcpy(text, state->data, totalLen);
  text[totalLen] = '\0';
  addMessageHistory(sourceId, text);
  *state = FragmentRxState{};
  fragmentRxDirty_ = true;
  return true;
}

bool LoRaManager::persistFragmentRx() {
  if (!storage.ready() || !fragmentRxDirty_) return true;
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;
  if (!SD.exists("/LORA") && !SD.mkdir("/LORA")) return false;
  const char* tmp = "/LORA/FRAG.NEW";
  const char* path = "/LORA/FRAG.Q";
  if (SD.exists(tmp)) SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;

  struct Header {
    uint16_t magic;
    uint8_t version;
    uint8_t slot;
    uint32_t sourceId;
    uint16_t messageId;
    uint8_t count;
    uint16_t totalLen;
    uint16_t receivedMask;
    uint16_t receivedBytes;
    uint32_t ageMs;
    uint16_t lengths[Config::LORA_FRAGMENT_MAX_COUNT];
  };

  for (uint8_t i = 0; i < FRAGMENT_RX_SLOTS; ++i) {
    const auto& st = fragmentRx_[i];
    if (!st.active) continue;
    Header hdr{};
    hdr.magic = FRAG_STORE_MAGIC;
    hdr.version = FRAG_STORE_VERSION;
    hdr.slot = i;
    hdr.sourceId = st.sourceId;
    hdr.messageId = st.messageId;
    hdr.count = st.count;
    hdr.totalLen = st.totalLen;
    hdr.receivedMask = st.receivedMask;
    hdr.receivedBytes = st.receivedBytes;
    hdr.ageMs = min<uint32_t>(Config::LORA_FRAGMENT_REASSEMBLY_TIMEOUT_MS + 1U,
                             millis() - st.startedMs);
    memcpy(hdr.lengths, st.lengths, sizeof(hdr.lengths));
    if (f.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) != sizeof(hdr) ||
        f.write(st.data, st.totalLen) != st.totalLen) {
      f.close();
      if (SD.exists(tmp)) SD.remove(tmp);
      return false;
    }
  }
  f.flush();
  f.close();
  const char* backup = "/LORA/FRAG.BAK";
  if (SD.exists(backup)) SD.remove(backup);
  if (SD.exists(path) && !SD.rename(path, backup)) {
    if (SD.exists(tmp)) SD.remove(tmp);
    return false;
  }
  if (!SD.rename(tmp, path)) {
    if (SD.exists(backup)) (void)SD.rename(backup, path);
    if (SD.exists(tmp)) SD.remove(tmp);
    return false;
  }
  if (SD.exists(backup)) SD.remove(backup);
  fragmentRxDirty_ = false;
  return true;
}

bool LoRaManager::loadFragmentRx() {
  if (!storage.ready()) return false;
  SpiLock spiLock(pdMS_TO_TICKS(500));
  if (!spiLock.ok()) return false;
  File f = SD.open("/LORA/FRAG.Q", FILE_READ);
  if (!f && SD.exists("/LORA/FRAG.BAK"))
    f = SD.open("/LORA/FRAG.BAK", FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return true;
  }
  struct Header {
    uint16_t magic;
    uint8_t version;
    uint8_t slot;
    uint32_t sourceId;
    uint16_t messageId;
    uint8_t count;
    uint16_t totalLen;
    uint16_t receivedMask;
    uint16_t receivedBytes;
    uint32_t ageMs;
    uint16_t lengths[Config::LORA_FRAGMENT_MAX_COUNT];
  };
  while (f.available()) {
    Header hdr{};
    if (f.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)) != sizeof(hdr) ||
        hdr.magic != FRAG_STORE_MAGIC ||
        (hdr.version != FRAG_STORE_VERSION && hdr.version != 1) ||
        hdr.slot >= FRAGMENT_RX_SLOTS || hdr.count == 0 ||
        hdr.count > Config::LORA_FRAGMENT_MAX_COUNT ||
        hdr.totalLen == 0 || hdr.totalLen > Config::LORA_FRAGMENT_MAX_BYTES ||
        hdr.receivedBytes > hdr.totalLen) {
      f.close();
      return false;
    }
    FragmentRxState state{};
    state.active = true;
    state.sourceId = hdr.sourceId;
    state.messageId = hdr.messageId;
    state.count = hdr.count;
    state.totalLen = hdr.totalLen;
    state.receivedMask = hdr.receivedMask;
    state.receivedBytes = hdr.receivedBytes;
    state.startedMs = millis() -
        min<uint32_t>(hdr.version == FRAG_STORE_VERSION ? hdr.ageMs :
                      Config::LORA_FRAGMENT_REASSEMBLY_TIMEOUT_MS + 1U,
                      Config::LORA_FRAGMENT_REASSEMBLY_TIMEOUT_MS + 1U);
    memcpy(state.lengths, hdr.lengths, sizeof(state.lengths));

    const uint16_t validMask = static_cast<uint16_t>((1UL << hdr.count) - 1UL);
    if ((state.receivedMask & static_cast<uint16_t>(~validMask)) != 0) {
      f.close();
      return false;
    }
    const size_t chunkMax = Config::LORA_MAX_PACKET - PACKET_HEADER_V2 -
                            PACKET_TAG - Config::LORA_FRAGMENT_HEADER_BYTES -
                            ROUTE_EXT_BYTES;
    uint32_t lengthSum = 0;
    for (uint8_t i = 0; i < hdr.count; ++i) {
      const uint16_t bit = static_cast<uint16_t>(1U << i);
      if (state.receivedMask & bit) {
        const size_t offset = static_cast<size_t>(i) * chunkMax;
        if (state.lengths[i] == 0 || state.lengths[i] > chunkMax ||
            offset >= state.totalLen ||
            state.lengths[i] > state.totalLen - offset) {
          f.close();
          return false;
        }
        lengthSum += state.lengths[i];
      } else if (state.lengths[i] != 0) {
        f.close();
        return false;
      }
    }
    for (uint8_t i = hdr.count; i < Config::LORA_FRAGMENT_MAX_COUNT; ++i) {
      if (state.lengths[i] != 0) {
        f.close();
        return false;
      }
    }
    if (lengthSum != state.receivedBytes) {
      f.close();
      return false;
    }
    if (f.read(state.data, state.totalLen) != state.totalLen) {
      f.close();
      return false;
    }
    fragmentRx_[hdr.slot] = state;
  }
  f.close();
  fragmentRxDirty_ = false;
  return true;
}

bool LoRaManager::enqueueTextFragments(const String& text, uint32_t destination) {
  if (text.length() <= Config::LORA_MAX_PACKET - PACKET_HEADER_TX - PACKET_TAG) return false;
  if (text.length() > Config::LORA_FRAGMENT_MAX_BYTES) return false;
  if (Config::LORA_MAX_PACKET <= PACKET_HEADER_TX + PACKET_TAG +
                                  Config::LORA_FRAGMENT_HEADER_BYTES + ROUTE_EXT_BYTES)
    return false;
  const size_t chunkMax = Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG -
                          Config::LORA_FRAGMENT_HEADER_BYTES - ROUTE_EXT_BYTES;
  if (chunkMax == 0 || chunkMax > Config::LORA_MAX_PACKET) return false;
  const uint8_t count = static_cast<uint8_t>((text.length() + chunkMax - 1) / chunkMax);
  if (count == 0 || count > Config::LORA_FRAGMENT_MAX_COUNT) return false;
  const uint16_t messageId = ++fragmentMessageId_;
  for (uint8_t index = 0; index < count; ++index) {
    const size_t offset = static_cast<size_t>(index) * chunkMax;
    const size_t chunkLen = min(chunkMax, text.length() - offset);
    uint8_t payload[Config::LORA_MAX_PACKET] = {};
    payload[0] = FRAGMENT_MAGIC; payload[1] = Config::LORA_FRAGMENT_VERSION;
    payload[2] = static_cast<uint8_t>(messageId & 0xFF); payload[3] = static_cast<uint8_t>(messageId >> 8);
    payload[4] = index; payload[5] = count;
    payload[6] = static_cast<uint8_t>(text.length() & 0xFF); payload[7] = static_cast<uint8_t>(text.length() >> 8);
    memcpy(payload + Config::LORA_FRAGMENT_HEADER_BYTES, text.c_str() + offset, chunkLen);
    uint16_t seq = 0;
    if (!nextTxSequence(seq)) return false;
    uint8_t routed[Config::LORA_MAX_PACKET] = {};
    const size_t routedLen = addRouteExtension(
        payload, Config::LORA_FRAGMENT_HEADER_BYTES + chunkLen, destination, 0,
        routed, sizeof(routed));
    String packet;
    if (!routedLen ||
        !encryptPacket(routed, routedLen, Config::LORA_TYPE_FRAG_DATA, seq, packet))
      return false;
    if (!textStateMutex_ ||
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(50)) != pdTRUE)
      return false;
    textPendingPacket_ = packet;
    textPendingSeq_ = seq;
    textRetryCount_ = 0;
    textSentMs_ = millis();
    textAwaitingAck_ = true;
    textAcked_ = false;
    xSemaphoreGive(textStateMutex_);

    if (!transmit(packet, true)) {
      if (xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        textAwaitingAck_ = false;
        textAcked_ = false;
        xSemaphoreGive(textStateMutex_);
      }
      return false;
    }

  }
  return true;
}

bool LoRaManager::sendSensorTelemetry(uint32_t nodeId, uint16_t sensorId,
                                        float value, uint8_t quality,
                                        uint64_t timestampMs) {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(20));
  if (!radioGuard.ok()) return false;
  if (!ready_ || nodeId == 0 || sensorId == 0 || !isfinite(value)) return false;

  uint8_t payload[Config::SENSOR_LORA_MAX_PAYLOAD] = {};
  if (SensorTelemetry::serializeSensorTelemetry(payload, nodeId, sensorId, value,
                                                  quality, timestampMs) !=
      Config::SENSOR_LORA_MAX_PAYLOAD) return false;
  // Keep the sensor record as an opaque binary payload. transmitHopped() adds
  // the authenticated routing envelope and consumes the normal duty budget.
  const String binary(reinterpret_cast<const char*>(payload),
                      Config::SENSOR_LORA_MAX_PAYLOAD);
  return transmitHopped(binary, Config::LORA_TYPE_SENSOR_TELEMETRY, 0);
}

bool LoRaManager::sendPosition() {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(20));
  if (!radioGuard.ok()) return false;
  double lat, lon, alt;
  uint32_t sat;
  {
    StateLock lock(gState);
    if (!lock.ok() || !gState.gps.valid) return false;
    lat = gState.gps.lat;
    lon = gState.gps.lon;
    alt = gState.gps.alt;
    sat = gState.gps.satellites;
  }

  (void)sat;
  return transmit(makePositionTelemetry());
}

bool LoRaManager::sendSOS() {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(20));
  if (!radioGuard.ok()) return false;
  if (sosAwaitingAck_ && millis() - sosSentMs_ < Config::SOS_REPEAT_MS)
    return false;
  {
    StateLock lock(gState);
    if (lock.ok() && (gState.sos || gState.sosEscalated)) return false;
  }

  bool valid;
  double lat, lon;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    valid = gState.gps.valid;
    lat = gState.gps.lat;
    lon = gState.gps.lon;
  }

  String plainText = "SOS,";
  plainText += valid ? String(lat, 6) + "," + String(lon, 6) : "NOFIX";
  plainText += ",TS=" + String(millis());

  struct FormatPayload { const uint8_t* data; size_t len; };
  uint8_t binary[16] = {};
  binary[0] = 'S'; binary[1] = 'O'; binary[2] = 'S'; binary[3] = 1;
  int32_t latE6 = valid ? static_cast<int32_t>(lat * 1000000.0) : 0;
  int32_t lonE6 = valid ? static_cast<int32_t>(lon * 1000000.0) : 0;
  memcpy(binary + 4, &latE6, 4);
  memcpy(binary + 8, &lonE6, 4);
  const uint32_t epoch = currentEpochSec();
  memcpy(binary + 12, &epoch, 4);

  const String aprs = valid
      ? ("!/" + String(lat, 4) + "," + String(lon, 4) + "/SOS")
      : "!/NOFIX/SOS";

  bool sentAny = false;
  uint16_t lastSeq = 0;
  String lastPacket;
  const uint8_t formats[3] = {1, 2, 4};
  for (uint8_t format : formats) {
    if (!(sosFormatMask_ & format)) continue;
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (format == 1) { data = reinterpret_cast<const uint8_t*>(plainText.c_str()); len = plainText.length(); }
    else if (format == 2) { data = reinterpret_cast<const uint8_t*>(aprs.c_str()); len = aprs.length(); }
    else { data = binary; len = sizeof(binary); }

    uint8_t routed[Config::LORA_MAX_PACKET] = {};
    const uint16_t seq = static_cast<uint16_t>(
        sosSeq_.fetch_add(1, std::memory_order_acq_rel) + 1U);
    const size_t routedLen = addRouteExtension(data, len, 0, 0, routed, sizeof(routed));
    String packet;
    if (!routedLen || !encryptPacket(routed, routedLen, Config::LORA_TYPE_SOS, seq, packet))
      continue;
    if (transmit(packet, true)) {
      sentAny = true;
      lastSeq = seq;
      lastPacket = packet;
    }
  }
  if (!sentAny) return false;

  sosPacket_ = lastPacket;
  sosSentMs_ = millis();
  sosRetryCount_ = 0;
  sosAwaitingAck_ = true;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.sosSeq = lastSeq;
      gState.sosAcked = false;
      gState.sosEscalated = false;
      gState.sosRetries = 0;
      gState.sosStartedMs = millis();
      gState.sosAckedBy = 0;
      gState.sos = true;
    }
  }
  addSosHistory(0);
  return true;
}


bool LoRaManager::cancelSOS() {
  sosAwaitingAck_ = false;
  sosPacket_ = String();
  bool wasActive = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    wasActive = gState.sos || gState.sosEscalated;
    gState.sos = false;
    gState.sosEscalated = false;
  }
  if (wasActive) addSosHistory(3);
  return wasActive;
}

bool LoRaManager::manualTune(float freqMHz) {
  RadioArbiterGuard radioGuard(radioArbiter, RadioOwner::LoRaP2P, pdMS_TO_TICKS(50));
  if (!radioGuard.ok()) return false;
  if (!isfinite(freqMHz) || freqMHz < Config::LORA_MIN_FREQ_MHZ ||
      freqMHz > Config::LORA_MAX_FREQ_MHZ || !mutex_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  bool ok = false;
  {
    SpiLock spiLock(pdMS_TO_TICKS(500));
    if (spiLock.ok()) ok = radio_.setFrequency(freqMHz) == RADIOLIB_ERR_NONE;
  }
  if (ok) {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.lastError = "";
    }
  }
  xSemaphoreGive(mutex_);
  return ok;
}
