#include "LoRaManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "PersistentConfig.h"
#include "AudioManager.h"
#include "Telemetry.h"
#include "StorageManager.h"
#include <esp_system.h>
#include <Preferences.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <time.h>
#include <esp_attr.h>
#include <SD.h>

extern AudioManager audio;
extern StorageManager storage;

namespace {
constexpr uint8_t PACKET_MAGIC = 0xF1;
constexpr size_t PACKET_HEADER_V1 = 1 + 1 + 1 + 2 + 4;
constexpr size_t PACKET_HEADER_V2 = PACKET_HEADER_V1 + 4 + 1;
constexpr size_t PACKET_HEADER_V3 = PACKET_HEADER_V2 + 1 + sizeof(uint32_t);
constexpr uint8_t ROUTE_EXT_MAGIC = 0xE7;
constexpr uint8_t ROUTE_EXT_VERSION = 1;
constexpr uint8_t ROUTE_EXT_FLAG_BROADCAST = 0x01;
constexpr uint8_t ROUTE_EXT_MAX_HOPS = Config::LORA_INITIAL_TTL;
constexpr uint32_t ROUTE_CACHE_TTL_MS = 120000UL;
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
constexpr uint16_t FORWARD_RECORD_MAGIC = 0x4C51;
constexpr size_t FORWARD_RECORD_FIXED_V1 = 2 + 1 + 1 + 1 + 1 + 2 + 4 + 2 + 4;
constexpr size_t FORWARD_RECORD_FIXED = FORWARD_RECORD_FIXED_V1 + 1;
constexpr uint8_t FORWARD_RECORD_VERSION = 2;

struct RtcRadioState {
  uint32_t magic;
  uint32_t hopFrame;
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
  const uint32_t now = millis();
  if (lastDutyRefillMs_ == 0) {
    lastDutyRefillMs_ = now;
    dutyTokensUs_ = (static_cast<uint64_t>(Config::LORA_DUTY_WINDOW_MS) *
                     Config::LORA_DUTY_CYCLE_PERCENT * 1000ULL) / 100ULL;
    return;
  }

  const uint32_t elapsedMs = now - lastDutyRefillMs_;
  if (!elapsedMs) return;

  const uint64_t maxBudget =
      (static_cast<uint64_t>(Config::LORA_DUTY_WINDOW_MS) *
       Config::LORA_DUTY_CYCLE_PERCENT * 1000ULL) / 100ULL;
  const uint64_t refill =
      (static_cast<uint64_t>(elapsedMs) * Config::LORA_DUTY_CYCLE_PERCENT *
       1000ULL) / 100ULL;
  dutyTokensUs_ = min(maxBudget, dutyTokensUs_ + refill);
  lastDutyRefillMs_ = now;
}

bool LoRaManager::consumeDutyBudget(uint32_t airtimeUs) {
  refillDutyBudget();
  if (airtimeUs == 0 || dutyTokensUs_ < airtimeUs) return false;
  dutyTokensUs_ -= airtimeUs;
  return true;
}


bool LoRaManager::loadKey(uint8_t key[16]) const {
  if (!key || gConfig.loraKeyHex.length() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    if (!hexByte(gConfig.loraKeyHex.c_str() + i * 2, key[i])) return false;
  }
  return true;
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
    const uint32_t age = currentEpoch >= packetEpochSec ? currentEpoch - packetEpochSec : packetEpochSec - currentEpoch;
    if (age > Config::LORA_REPLAY_TIME_WINDOW_SEC) return true;
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
    return false;
  }

  const int16_t delta = static_cast<int16_t>(seq - slot->highestSeq);
  if (delta > 0) {
    const uint8_t shift = static_cast<uint8_t>(min<int16_t>(delta, Config::LORA_REPLAY_WINDOW_BITS));
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
  if (delta == 0) return true;

  const int16_t age = static_cast<int16_t>(slot->highestSeq - seq);
  if (age >= Config::LORA_REPLAY_WINDOW_BITS) return true;
  const uint32_t bit = 1UL << age;
  if (slot->bitmap & bit) return true;
  slot->bitmap |= bit;
  slot->seenMs = now;
  slot->lastEpochSec = packetEpochSec;
  return false;
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
  uint8_t key[16];
  if (!plain || !loadKey(key) ||
      len + PACKET_HEADER_V2 + PACKET_TAG > Config::LORA_MAX_PACKET)
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
                                   uint16_t seq, uint8_t hopIndex, uint32_t epochMs,
                                   String& packet) {
  uint8_t key[16];
  if (!plain || !loadKey(key) ||
      len + PACKET_HEADER_V3 + PACKET_TAG > Config::LORA_MAX_PACKET)
    return false;
  uint8_t rotatingKey[16] = {};
  if (epochMs != 0 && deriveRotatingKey(key, epochMs, rotatingKey)) memcpy(key, rotatingKey, sizeof(key));

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
    packet += static_cast<char>((epochMs >> (8 * i)) & 0xFF);

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &seq, sizeof(seq));
  iv[6] = hopIndex;
  memcpy(iv + 8, &epochMs, sizeof(epochMs));
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
                                  uint32_t& epochMs, uint8_t* plain,
                                  size_t capacity, size_t& len) {
  len = 0; sourceId = 0; ttl = 0; hopIndex = 0; epochMs = 0;
  if (packet.length() < PACKET_HEADER_V3 + PACKET_TAG ||
      static_cast<uint8_t>(packet[0]) != PACKET_MAGIC ||
      static_cast<uint8_t>(packet[1]) != LORA_PROTOCOL_VERSION_HOP)
    return false;

  type = static_cast<uint8_t>(packet[2]);
  seq = static_cast<uint16_t>(static_cast<uint8_t>(packet[3])) |
        (static_cast<uint16_t>(static_cast<uint8_t>(packet[4])) << 8);
  uint32_t nonce = 0;
  memcpy(&nonce, packet.c_str() + 5, sizeof(nonce));
  memcpy(&sourceId, packet.c_str() + 9, sizeof(sourceId));
  ttl = static_cast<uint8_t>(packet[13]);
  hopIndex = static_cast<uint8_t>(packet[14]);
  memcpy(&epochMs, packet.c_str() + 15, sizeof(epochMs));

  const size_t cipherLen = packet.length() - PACKET_HEADER_V3 - PACKET_TAG;
  if (!plain || cipherLen > capacity) return false;

  uint8_t key[16];
  if (!loadKey(key)) return false;
  uint8_t rotatingKey[16] = {};
  if (epochMs != 0 && deriveRotatingKey(key, epochMs, rotatingKey)) memcpy(key, rotatingKey, sizeof(key));
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
  memcpy(iv + 8, &epochMs, sizeof(epochMs));
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

uint8_t LoRaManager::computeHopIndex(uint32_t frame) const {
  StateLock lock(gState);
  if (!lock.ok() || gState.hopChannelCount == 0) return 0;
  const uint32_t epochSec = currentEpochSec();
  if (epochSec != 0) {
    const uint32_t dwellSec = max<uint32_t>(1, Config::HOP_DWELL_MS / 1000U);
    return static_cast<uint8_t>((epochSec / dwellSec) % gState.hopChannelCount);
  }
  return static_cast<uint8_t>(frame % gState.hopChannelCount);
}

bool LoRaManager::retuneToHopChannel(uint8_t index) {
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

bool LoRaManager::retuneToChannel0() {
  SpiLock spiLock(pdMS_TO_TICKS(1000));
  if (!spiLock.ok()) return false;
  return radio_.setFrequency(gConfig.loraFreqMHz) == RADIOLIB_ERR_NONE &&
         radio_.startReceive() == RADIOLIB_ERR_NONE;
}

bool LoRaManager::transmitHopped(const String& text, uint8_t type, uint32_t destination) {
  if (!ready_ || !mutex_ || text.isEmpty()) return false;
  bool hopOn = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    hopOn = gState.hopEnabled && gState.hopChannelCount > 0;
  }
  if (text.length() + ROUTE_EXT_BYTES > Config::LORA_MAX_PACKET - PACKET_HEADER_V3 - PACKET_TAG) return false;
  const uint8_t hopIndex = hopOn ? computeHopIndex(hopFrame_) : 0;
  const uint32_t epochSec = currentEpochSec();
  uint16_t seq = 0;
  if (!nextTxSequence(seq)) return false;
  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const size_t routedLen = addRouteExtension(
      reinterpret_cast<const uint8_t*>(text.c_str()), text.length(),
      destination, 0, routed, sizeof(routed));
  if (!routedLen) return false;
  String packet;
  if (!encryptPacketV3(routed, routedLen, type, seq, hopIndex, epochSec, packet)) return false;

  if (type == Config::LORA_TYPE_TEXT) {
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
  if (hopOn && !retuneToHopChannel(hopIndex)) {
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
          (void)radio_.startReceive();
        } else st = RADIOLIB_ERR_UNKNOWN;
      } else st = scanSt;
    }
    if (txOk || attempt >= Config::LORA_LBT_MAX_RETRIES) break;
    const uint32_t span = Config::LORA_LBT_BACKOFF_MAX_MS - Config::LORA_LBT_BACKOFF_MIN_MS;
    vTaskDelay(pdMS_TO_TICKS(Config::LORA_LBT_BACKOFF_MIN_MS + (span ? (esp_random() % (span + 1U)) : 0U)));
  }
  xSemaphoreGive(mutex_);
  (void)retuneToChannel0();
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
  logPacket(true, type, seq, sourceId_, 0, 0.0f, Config::LORA_INITIAL_TTL);
  return true;
}

bool LoRaManager::decryptPacket(const String& packet, uint8_t& type, uint16_t& seq,
                                uint32_t& sourceId, uint8_t& ttl,
                                uint8_t* plain, size_t capacity, size_t& len) {
  len = 0;
  sourceId = 0;
  ttl = 0;
  size_t headerLen = 0;

  if (packet.length() < PACKET_HEADER_V1 + PACKET_TAG ||
      static_cast<uint8_t>(packet[0]) != PACKET_MAGIC)
    return false;

  const uint8_t version = static_cast<uint8_t>(packet[1]);
  if (version == Config::LORA_PROTOCOL_VERSION) {
    if (packet.length() < PACKET_HEADER_V2 + PACKET_TAG) return false;
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

  uint8_t key[16];
  if (!loadKey(key)) return false;
  type = static_cast<uint8_t>(packet[2]);
  seq = static_cast<uint16_t>(static_cast<uint8_t>(packet[3])) |
        (static_cast<uint16_t>(static_cast<uint8_t>(packet[4])) << 8);
  uint32_t nonce = 0;
  memcpy(&nonce, packet.c_str() + 5, sizeof(nonce));

  unsigned char expected[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
      reinterpret_cast<const unsigned char*>(packet.c_str()),
      headerLen + cipherLen, expected, sizeof(expected)) != 0)
    return false;

  const uint8_t* got = reinterpret_cast<const uint8_t*>(packet.c_str()) + headerLen + cipherLen;
  uint8_t diff = 0;
  for (size_t i = 0; i < PACKET_TAG; ++i) diff |= expected[i] ^ got[i];
  if (diff != 0) return false;

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &seq, sizeof(seq));
  uint8_t streamBlock[16] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
      mbedtls_aes_crypt_ctr(&aes, cipherLen, &ncOff, iv, streamBlock,
          reinterpret_cast<const unsigned char*>(packet.c_str()) + headerLen, plain) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) return false;
  len = cipherLen;

  return true;
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
      return true;
    }
  }

  DedupEntry& slot = dedupCache_[dedupNext_];
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
  StateLock lock(gState);
  if (!lock.ok()) return;
  MessageHistoryEntry& e = gState.messageHistory[gState.messageHistoryNext];
  e.timestamp = gState.gps.timeValid ? gState.gps.utcEpoch : millis();
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
       wireVersion != LORA_PROTOCOL_VERSION_HOP) ||
      len > Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG)
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
        hopCount >= ROUTE_EXT_MAX_HOPS) return false;
  }

  // Queue the authenticated payload exactly as received. Route mutation must
  // happen once, immediately before transmission, when the next hop is known.
  // Rewriting it here and again in transmitForward() makes previousHop become
  // this node and causes routeAllowsForward() to reject our own queued packet.
  if (len > Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG)
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
  if (xQueueSend(forwardQueue_, &packet, 0) != pdPASS) return false;
  (void)persistForwardQueue();
  return true;
}

bool LoRaManager::persistForwardQueue() {
  if (!storage.ready() || !forwardQueue_) return false;
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
    if (!item.len || item.len > Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG) continue;
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
  if (SD.exists(FORWARD_QUEUE_FILE)) SD.remove(FORWARD_QUEUE_FILE);
  if (!SD.rename(tmpPath, FORWARD_QUEUE_FILE)) {
    if (SD.exists(tmpPath)) SD.remove(tmpPath);
    restoreQueue();
    return false;
  }
  restoreQueue();
  return true;
}

bool LoRaManager::loadForwardQueue() {
  if (!storage.ready() || !forwardQueue_ || !SD.exists(FORWARD_QUEUE_FILE)) return true;
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;
  File f = SD.open(FORWARD_QUEUE_FILE, FILE_READ);
  if (!f) return false;
  uint8_t header[FORWARD_RECORD_FIXED] = {};
  uint8_t key[16] = {};
  if (!loadKey(key)) { f.close(); return false; }
  size_t loaded = 0;
  while (loaded < Config::LORA_STORE_FORWARD_MAX_RECORDS &&
         f.available() >= static_cast<int>(FORWARD_RECORD_FIXED_V1)) {
    memset(header, 0, sizeof(header));
    if (f.read(header, FORWARD_RECORD_FIXED_V1) != FORWARD_RECORD_FIXED_V1) break;

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
      if (f.read(&wireVersion, 1) != 1) break;
      header[FORWARD_RECORD_FIXED_V1] = wireVersion;
      headerLen = FORWARD_RECORD_FIXED;
    } else if (version != 1) {
      break;
    }

    if (magic != FORWARD_RECORD_MAGIC || ttl == 0 || len == 0 ||
        len > Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG ||
        (wireVersion != Config::LORA_PROTOCOL_VERSION &&
         wireVersion != LORA_PROTOCOL_VERSION_HOP))
      break;

    uint8_t cipher[Config::LORA_MAX_PACKET] = {};
    uint8_t tag[PACKET_TAG] = {};
    if (f.read(cipher, len) != len || f.read(tag, PACKET_TAG) != PACKET_TAG) break;

    uint8_t bind[Config::LORA_MAX_PACKET + FORWARD_RECORD_FIXED] = {};
    memcpy(bind, header, headerLen);
    memcpy(bind + headerLen, cipher, len);
    uint8_t expected[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, key, sizeof(key), bind, headerLen + len,
                               expected, sizeof(expected)) != 0) break;
    uint8_t diff = 0;
    for (size_t i = 0; i < PACKET_TAG; ++i) diff |= expected[i] ^ tag[i];
    if (diff != 0) continue;

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
    if (!ok) continue;

    ForwardPacket item{};
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
    if (xQueueSend(forwardQueue_, &item, 0) == pdPASS) ++loaded;
  }
  f.close();
  return true;
}

bool LoRaManager::parseRouteExtension(const uint8_t* payload, size_t len,
                                       uint32_t& destination, uint32_t& nextHop,
                                       uint32_t& previousHop, uint8_t& hopCount,
                                       size_t& payloadOffset) const {
  destination = nextHop = previousHop = 0;
  hopCount = 0;
  payloadOffset = 0;
  if (!payload || len < ROUTE_EXT_BYTES ||
      payload[0] != ROUTE_EXT_MAGIC || payload[1] != ROUTE_EXT_VERSION)
    return false;
  const uint8_t flags = payload[2];
  hopCount = payload[3];
  memcpy(&destination, payload + 4, 4);
  memcpy(&nextHop, payload + 8, 4);
  memcpy(&previousHop, payload + 12, 4);
  if (hopCount > ROUTE_EXT_MAX_HOPS) return false;
  if ((flags & static_cast<uint8_t>(~ROUTE_EXT_FLAG_BROADCAST)) != 0) return false;
  if ((flags & ROUTE_EXT_FLAG_BROADCAST) != 0 && destination != 0) return false;
  if (destination == sourceId_ || nextHop == sourceId_ || nextHop == 0) {
    payloadOffset = ROUTE_EXT_BYTES;
    return true;
  }
  payloadOffset = ROUTE_EXT_BYTES;
  return true;
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
        if (n.sourceId == route.nextHop && now - n.seenMs <= ROUTE_CACHE_TTL_MS) {
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
          now - n.seenMs > ROUTE_CACHE_TTL_MS) continue;
      const uint32_t etxQ8 = n.txAttempts && n.txSuccess
          ? static_cast<uint32_t>(256UL * n.txAttempts / max<uint16_t>(1, n.txSuccess))
          : 256U;
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
        now - n.seenMs > ROUTE_CACHE_TTL_MS) continue;
    const uint32_t etxQ8 = n.txAttempts && n.txSuccess
        ? static_cast<uint32_t>(256UL * n.txAttempts / max<uint16_t>(1, n.txSuccess))
        : 256U;
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
                                       uint8_t* out, size_t capacity) const {
  if (!payload || !out || destination == sourceId_ ||
      len + ROUTE_EXT_BYTES > capacity) return 0;
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
  memcpy(out + 12, &sourceId_, 4);
  memcpy(out + ROUTE_EXT_BYTES, payload, len);
  return len + ROUTE_EXT_BYTES;
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
    if (n.sourceId == nextHop && now - n.seenMs <= ROUTE_CACHE_TTL_MS) {
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
    if (n.sourceId == nextHop && n.txAttempts && n.txSuccess) {
      route.etxQ8 = static_cast<uint16_t>(
          min<uint32_t>(ROUTE_ETX_MAX_Q8,
                        256UL * n.txAttempts / max<uint16_t>(1, n.txSuccess)));
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
    return;
  }
}

bool LoRaManager::isPttOrRecording() const {
  StateLock lock(gState);
  return !lock.ok() || gState.ptt || gState.recording;
}

bool LoRaManager::transmitForward(const ForwardPacket& forward) {
  if (!ready_ || !mutex_ || !forward.len || forward.ttl == 0 ||
      forward.len > Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG ||
      isPttOrRecording())
    return false;

  String packet;

  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  size_t routedLen = 0;
  uint32_t destination = 0;
  uint32_t nextHop = 0;
  uint32_t previousHop = 0;
  uint8_t hopCount = 0;
  size_t routeOffset = 0;
  const bool hasRoute = parseRouteExtension(
      forward.payload, forward.len, destination, nextHop, previousHop,
      hopCount, routeOffset);
  if (hasRoute) {
    if (!routeAllowsForward(destination, nextHop, previousHop) ||
        hopCount >= ROUTE_EXT_MAX_HOPS) return false;
    const uint32_t selectedNextHop = selectNextHop(destination, previousHop);
    if (selectedNextHop == 0 || selectedNextHop == previousHop) return false;
    routed[0] = ROUTE_EXT_MAGIC;
    routed[1] = ROUTE_EXT_VERSION;
    routed[2] = destination == 0 ? ROUTE_EXT_FLAG_BROADCAST : 0;
    routed[3] = static_cast<uint8_t>(hopCount + 1U);
    memcpy(routed + 4, &destination, 4);
    memcpy(routed + 8, &selectedNextHop, 4);
    memcpy(routed + 12, &sourceId_, 4);
    const size_t appLen = forward.len - routeOffset;
    if (routeOffset != ROUTE_EXT_BYTES ||
        appLen + ROUTE_EXT_BYTES > sizeof(routed)) return false;
    memcpy(routed + ROUTE_EXT_BYTES, forward.payload + routeOffset, appLen);
    routedLen = appLen + ROUTE_EXT_BYTES;
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
    if (!queuePendingTx(packet, forward.priority)) return false;
    // Keep the original queued packet for retry/persistence. The routed
    // envelope is only the wire representation; re-queuing that representation
    // after an LBT failure would make previousHop == this node and reject the
    // packet on the next forwarding attempt.
    forwardInFlight_ = forward;
    forwardInFlightNextHop_ = nextHop;
    forwardInFlightActive_ = true;
    (void)processPendingTx();
    return true;
  }

  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  if (useV3 && !retuneToHopChannel(txHopIndex)) {
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
          dutyTokensUs_ = min(
              (static_cast<uint64_t>(Config::LORA_DUTY_WINDOW_MS) *
               Config::LORA_DUTY_CYCLE_PERCENT * 1000ULL) / 100ULL,
              dutyTokensUs_ + static_cast<uint64_t>(airtimeUs));
      } else {
        st = -1;
      }
    } else {
      st = -1;
    }
  }

  const bool txOk = budgetConsumed && st == RADIOLIB_ERR_NONE;
  if (hasRoute) recordNeighborTxResult(nextHop, txOk);
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
  xSemaphoreGive(mutex_);
  if (useV3) (void)retuneToChannel0();
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
  for (auto& result : scanner_.results) result = ScannerState::Result{};
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.scannerActive = true;
      gState.scannerMode = mode;
      gState.scannerSweepCount = 0;
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
  (void)retuneToChannel0();
  {
    StateLock lock(gState);
    if (lock.ok()) gState.scannerActive = false;
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
    if (scanner_.results[i].timestamp == 0) continue;
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
      if (r.timestamp == 0) continue;
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
  e.seq = sosSeq_;
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
  if (transmit(sosPacket_, true)) {
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

void LoRaManager::serviceTextRetry() {
  String packet;
  uint32_t sentMs = 0;
  uint8_t retryCount = 0;
  {
    if (!textStateMutex_ ||
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (!textAwaitingAck_ || textPendingPacket_.isEmpty() ||
        millis() - textSentMs_ < Config::SOS_REPEAT_MS) {
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
    packet = textPendingPacket_;
    sentMs = textSentMs_;
    retryCount = textRetryCount_;
    xSemaphoreGive(textStateMutex_);
  }

  if (transmit(packet, true)) {
    if (textStateMutex_ &&
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
      // Only update the retry timestamp if the same pending packet is still
      // active; an ACK may have arrived while transmit() was running.
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
  if (!sosAwaitingAck_ || ackedSeq != sosSeq_ || ackedSource != sourceId_) return;
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

bool LoRaManager::begin() {
  instance_ = this;
  mutex_ = xSemaphoreCreateMutex();
  seqMutex_ = xSemaphoreCreateMutex();
  textStateMutex_ = xSemaphoreCreateMutex();
  forwardQueue_ = xQueueCreateStatic(FORWARD_QUEUE_DEPTH, sizeof(ForwardPacket),
                                     forwardQueueStorage_, &forwardQueueStruct_);
  sourceId_ = sourceIdFromCallsign(gConfig.callsign);
  if (rtcRadioState.magic == RTC_RADIO_MAGIC && rtcRadioState.crc == stateCrc(rtcRadioState)) {
    hopFrame_ = rtcRadioState.hopFrame;
    sosSeq_ = rtcRadioState.sosSeq;
    sosRetryCount_ = rtcRadioState.sosRetryCount;
    sosAwaitingAck_ = rtcRadioState.sosAwaitingAck;
    if (sosAwaitingAck_ && rtcRadioState.sosPacketLen > 0 &&
        rtcRadioState.sosPacketLen <= Config::LORA_MAX_PACKET) {
      sosPacket_ = String(reinterpret_cast<const char*>(rtcRadioState.sosPacket), rtcRadioState.sosPacketLen);
      sosSentMs_ = millis() - min<uint32_t>(rtcRadioState.sosElapsedMs, 0x7FFFFFFFU);
    }
  }
  if (!mutex_ || !seqMutex_ || !textStateMutex_ || !forwardQueue_ || !reserveTxSequenceBlock()) return false;
  (void)loadForwardQueue();

  SpiLock spiLock(pdMS_TO_TICKS(1000));
  if (!spiLock.ok()) return false;

  int16_t st = radio_.begin(
      gConfig.loraFreqMHz, gConfig.loraBwKHz, gConfig.loraSf,
      gConfig.loraCr, gConfig.loraSyncWord, effectiveTxPowerDbm(),
      Config::LORA_PREAMBLE, Config::LORA_TCXO_VOLTAGE);

  if (st != RADIOLIB_ERR_NONE) {
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
  if (!mutex_) return;

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
      if (scanner_.mode == 1) scanner_.active = false;
      if (scanner_.active) scanner_.lastSampleMs = now;
    } else {
      scanner_.lastSampleMs = now;
    }
    if (!scanner_.active) {
      (void)retuneToChannel0();
      StateLock lock(gState);
      if (lock.ok()) {
        gState.scannerActive = false;
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
        if (legacyRxCounter_ >= Config::HOP_LEGACY_RX_EVERY) {
          legacyRxCounter_ = 0;
          const uint8_t idx = computeHopIndex(hopFrame_);
          if (!retuneToHopChannel(idx)) {
            StateLock lock(gState);
            if (lock.ok()) gState.lastError = "hop retune failed";
            return;
          }
          currentHopIndex_ = idx;
          ++hopFrame_;
        } else {
          (void)retuneToChannel0();
          ++legacyRxCounter_;
        }
        hopLastSyncMs_ = now;
      }
    }
  }

  (void)processPendingTx();
  serviceSosRetry();
  serviceTextRetry();
  serviceVoiceAckRetry();
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
        millis() - lastForwardTxMs_ >= Config::LORA_FORWARD_RATE_LIMIT_MS) {
      ForwardPacket forward{};
      if (forwardQueue_ && xQueueReceive(forwardQueue_, &forward, 0) == pdPASS) {
        if (transmitForward(forward)) lastForwardTxMs_ = millis();
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
    uint32_t epochMs = 0;
    bool authenticatedV3 = false;
    if (!authenticated) {
      authenticatedV3 = decryptPacketV3(msg, type, seq, rxSourceId, rxTtl,
                                        hopIndex, epochMs, plain,
                                        sizeof(plain), plainLen);
      if (authenticatedV3) {
        currentHopIndex_ = hopIndex;
        hopLastSyncMs_ = millis();
      }
    }
    const bool rxAuthenticated = authenticated || authenticatedV3;
    const bool isV2 = authenticated &&
                     static_cast<uint8_t>(msg[1]) == Config::LORA_PROTOCOL_VERSION;
    const bool isV3 = authenticatedV3;
    uint32_t routeDestination = 0;
    uint32_t routeNextHop = 0;
    uint32_t routePreviousHop = 0;
    uint8_t routeHopCount = 0;
    size_t routeOffset = 0;
    const bool routePrefixPresent = rxAuthenticated && plainLen >= 2 &&
        plain[0] == ROUTE_EXT_MAGIC && plain[1] == ROUTE_EXT_VERSION;
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
    const bool duplicateV2 = (isV2 || isV3) &&
        seenDedup(rxSourceId, seq, type, hashPayload(plain, plainLen), isV3 ? epochMs : 0);
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
    const bool isForwardable = rxAuthenticated && (isV2 || isV3) &&
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
        nextHopIsUs && type == Config::LORA_TYPE_TEXT && textPayloadValid) {
      textAckSeq_ = seq;
      textAckSourceId_ = rxSourceId;
      textAckHopIndex_ = isV3 ? hopIndex : 0;
      textAckPending_ = true;
    }
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_SOS && appPayloadLen > 0 &&
        !duplicateV2) {
      char text[Config::LORA_MAX_PACKET + 1] = {};
      memcpy(text, appPayload, appPayloadLen);
      text[appPayloadLen] = '\0';
      addMessageHistory(rxSourceId, text);
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
    if (rxAuthenticated && addressedToUs && type == Config::LORA_TYPE_VOICE && !duplicateV2 && appPayloadLen == 168 &&
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
        if (!alreadyQueued) {
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
      (void)enqueueForward(type, seq, rxSourceId, rxTtl, plain, plainLen,
                            isV3 ? LORA_PROTOCOL_VERSION_HOP : Config::LORA_PROTOCOL_VERSION);
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
      millis() - lastForwardTxMs_ >= Config::LORA_FORWARD_RATE_LIMIT_MS) {
    ForwardPacket forward{};
    if (forwardQueue_ && xQueueReceive(forwardQueue_, &forward, 0) == pdPASS) {
      if (transmitForward(forward)) lastForwardTxMs_ = millis();
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
      static_cast<uint8_t>(pendingTx_.packet[1]) == LORA_PROTOCOL_VERSION_HOP;
  const uint8_t pendingHopIndex = pendingHopped
      ? static_cast<uint8_t>(pendingTx_.packet[14]) : 0;
  if (pendingHopped && !retuneToHopChannel(pendingHopIndex)) {
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
          if (!txOk && budgetConsumed)
            dutyTokensUs_ = min(
                (static_cast<uint64_t>(Config::LORA_DUTY_WINDOW_MS) *
                 Config::LORA_DUTY_CYCLE_PERCENT * 1000ULL) / 100ULL,
                dutyTokensUs_ + static_cast<uint64_t>(airtimeUs));
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
  }

  if (!pendingError.isEmpty()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = pendingError;
  }

  if (!txOk && done && forwardInFlightActive_) {
    if (forwardInFlightNextHop_ != 0)
      recordNeighborTxResult(forwardInFlightNextHop_, false);
    if (xQueueSend(forwardQueue_, &forwardInFlight_, 0) != pdPASS) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa forward retry queue full";
    }
    forwardInFlightActive_ = false;
    forwardInFlightNextHop_ = 0;
    (void)persistForwardQueue();
  }
  if (txOk && forwardInFlightActive_) {
    if (forwardInFlightNextHop_ != 0)
      recordNeighborTxResult(forwardInFlightNextHop_, true);
    forwardInFlightActive_ = false;
    forwardInFlightNextHop_ = 0;
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
  xSemaphoreGive(mutex_);
  if (pendingHopped) (void)retuneToChannel0();
  return done;
}

bool LoRaManager::transmit(const String& text, bool alreadyEncrypted) {
  if (!ready_ || !mutex_ || text.isEmpty() ||
      text.length() > (alreadyEncrypted
          ? Config::LORA_MAX_PACKET
          : Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG - ROUTE_EXT_BYTES))
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
        wireVersion != LORA_PROTOCOL_VERSION_HOP)
      return false;
    txType = static_cast<uint8_t>(packet[2]);
    txSeq = static_cast<uint16_t>(static_cast<uint8_t>(packet[3])) |
            (static_cast<uint16_t>(static_cast<uint8_t>(packet[4])) << 8);
    txTtl = static_cast<uint8_t>(packet[13]);
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
      static_cast<uint8_t>(packet[1]) == LORA_PROTOCOL_VERSION_HOP;
  const uint8_t packetHopIndex = hoppedPacket
      ? static_cast<uint8_t>(packet[14]) : 0;
  if (hoppedPacket && !retuneToHopChannel(packetHopIndex)) {
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
    const uint32_t txElapsedMs = millis() - txStartMs;
    if (txElapsedMs > Config::LORA_TX_TIMEOUT_MS) {
      ready_ = false;
      st = RADIOLIB_ERR_TX_TIMEOUT;
    }
    rxSt = radio_.startReceive();

    if (st != RADIOLIB_ERR_NONE && dutyTokensUs_ <= UINT32_MAX) {
      dutyTokensUs_ = min(
          (static_cast<uint64_t>(Config::LORA_DUTY_WINDOW_MS) *
           Config::LORA_DUTY_CYCLE_PERCENT * 1000ULL) / 100ULL,
          dutyTokensUs_ + static_cast<uint64_t>(airtimeUs));
    }
  }

  const bool ok = (st == RADIOLIB_ERR_NONE);
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
  if (hoppedPacket) (void)retuneToChannel0();
  return ok;
}

bool LoRaManager::prepareForDeepSleep() {
  rtcRadioState.magic = RTC_RADIO_MAGIC;
  rtcRadioState.hopFrame = hopFrame_;
  rtcRadioState.sosSeq = sosSeq_;
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

  if (!Config::LORA_RX_DUTY_CYCLE_ENABLED || !ready_ || !mutex_) return ready_;

  // Keep the same lock order used by task(): mutex -> SPI. This prevents
  // entering deep sleep while another task is using the SX1262.
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

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
      }
    }
  }

  xSemaphoreGive(mutex_);
  return armed;
}

bool LoRaManager::applyConfig() {
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
  return sendTextTo(0, text);
}

bool LoRaManager::sendTextTo(uint32_t destination, const String& text) {
  if (destination == sourceId_) return false;
  if (text.length() > Config::LORA_MAX_PACKET - PACKET_HEADER_V3 - PACKET_TAG - ROUTE_EXT_BYTES)
    return enqueueTextFragments(text, destination);
  if (!transmitHopped(text, Config::LORA_TYPE_TEXT, destination)) return false;

  const uint32_t retryWindow =
      Config::SOS_REPEAT_MS * (static_cast<uint32_t>(Config::SOS_MAX_RETRIES) + 1U);
  const uint32_t deadline = millis() + retryWindow + 500U;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    bool awaiting = false;
    bool acked = false;
    if (textStateMutex_ &&
        xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
      awaiting = textAwaitingAck_;
      acked = textAcked_;
      xSemaphoreGive(textStateMutex_);
    }
    if (!awaiting || acked) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  bool acked = false;
  bool awaiting = false;
  if (textStateMutex_ &&
      xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    acked = textAcked_;
    awaiting = textAwaitingAck_;
    xSemaphoreGive(textStateMutex_);
  }
  if (!acked) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = awaiting
        ? "Text transmitted; ACK not received"
        : "Text transmission failed or timed out";
  }
  return acked;
}

bool LoRaManager::sendVoiceFrame() {
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
  for (const auto& entry : neighbors_) {
    if (entry.sourceId == sourceId && millis() - entry.seenMs <= 120000UL)
      return entry.quality;
  }
  return 0;
}

uint8_t LoRaManager::bestNeighborQuality() const {
  uint8_t best = 0;
  const uint32_t now = millis();
  for (const auto& entry : neighbors_) {
    if (entry.sourceId != 0 && now - entry.seenMs <= ROUTE_CACHE_TTL_MS)
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
    const uint16_t behind = static_cast<uint16_t>(ackBase - slot.seq);
    bool acked = behind >= 1U && behind <= Config::LORA_VOICE_WINDOW_SIZE;
    if (!acked) {
      const uint16_t ahead = static_cast<uint16_t>(slot.seq - ackBase);
      acked = ahead >= 1U && ahead <= Config::LORA_VOICE_WINDOW_SIZE &&
              (ackBitmap & (1U << (ahead - 1U)));
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
    if (neighbor.seenMs != 0 && now - neighbor.seenMs > ROUTE_CACHE_TTL_MS)
      neighbor = NeighborEntry{};
  }
  if (!ready_ || now - lastNeighborBeaconMs_ < Config::LORA_NEIGHBOR_BEACON_PERIOD_MS)
    return;
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
  if (Config::LORA_MAX_PACKET <= PACKET_HEADER_V2 + PACKET_TAG +
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
  for (auto& candidate : fragmentRx_) {
    if (candidate.active &&
        now - candidate.startedMs > Config::LORA_FRAGMENT_REASSEMBLY_TIMEOUT_MS)
      candidate = FragmentRxState{};
    if (candidate.active && candidate.sourceId == sourceId &&
        candidate.messageId == messageId) {
      state = &candidate;
      break;
    }
    if (!candidate.active && !reusable) reusable = &candidate;
  }
  if (!state) {
    state = reusable;
    if (!state) {
      // All slots are busy. Replace the oldest incomplete message rather than
      // corrupting an arbitrary active reassembly.
      state = &fragmentRx_[0];
      for (auto& candidate : fragmentRx_) {
        if (static_cast<int32_t>(candidate.startedMs - state->startedMs) < 0)
          state = &candidate;
      }
    }
    *state = FragmentRxState{};
    state->active = true;
    state->sourceId = sourceId;
    state->messageId = messageId;
    state->count = count;
    state->totalLen = totalLen;
    state->startedMs = now;
  } else if (state->count != count || state->totalLen != totalLen) {
    // Same (source,messageId) with conflicting authenticated metadata is not a
    // continuation of the current message; reject it instead of resetting a
    // valid in-flight reassembly.
    return false;
  }

  const uint16_t bit = static_cast<uint16_t>(1U << index);
  if (!(state->receivedMask & bit)) {
    memcpy(state->data + offset, payload + Config::LORA_FRAGMENT_HEADER_BYTES, chunkLen);
    state->lengths[index] = static_cast<uint16_t>(chunkLen);
    state->receivedMask |= bit;
    state->receivedBytes = static_cast<uint16_t>(state->receivedBytes + chunkLen);
  } else if (state->lengths[index] != chunkLen ||
             memcmp(state->data + offset,
                    payload + Config::LORA_FRAGMENT_HEADER_BYTES, chunkLen) != 0) {
    // A duplicate fragment must be byte-identical. This catches conflicting
    // fragment variants without destroying the already authenticated state.
    return false;
  }
  const uint16_t completeMask = static_cast<uint16_t>((1UL << count) - 1UL);
  if (state->receivedMask != completeMask || state->receivedBytes != totalLen) return true;
  char text[Config::LORA_FRAGMENT_MAX_BYTES + 1] = {};
  memcpy(text, state->data, totalLen);
  text[totalLen] = '\0';
  addMessageHistory(sourceId, text);
  *state = FragmentRxState{};
  return true;
}

bool LoRaManager::enqueueTextFragments(const String& text, uint32_t destination) {
  if (text.length() <= Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG) return false;
  if (text.length() > Config::LORA_FRAGMENT_MAX_BYTES) return false;
  if (Config::LORA_MAX_PACKET <= PACKET_HEADER_V2 + PACKET_TAG +
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
        !encryptPacket(routed, routedLen, Config::LORA_TYPE_TEXT, seq, packet))
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

    const uint32_t retryWindow =
        Config::SOS_REPEAT_MS * (static_cast<uint32_t>(Config::SOS_MAX_RETRIES) + 1U);
    const uint32_t deadline = millis() + retryWindow + 500U;
    while (static_cast<int32_t>(millis() - deadline) < 0) {
      bool awaiting = false;
      bool acked = false;
      if (xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        awaiting = textAwaitingAck_;
        acked = textAcked_;
        xSemaphoreGive(textStateMutex_);
      }
      if (!awaiting || acked) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool acked = false;
    if (xSemaphoreTake(textStateMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
      acked = textAcked_;
      xSemaphoreGive(textStateMutex_);
    }
    if (!acked) return false;
  }
  return true;
}

bool LoRaManager::sendPosition() {
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
  // SOS retries are owned by the retry state machine. Reject a second manual
  // trigger while an SOS is already active to avoid flooding the channel.
  if (sosAwaitingAck_ && millis() - sosSentMs_ < Config::SOS_REPEAT_MS)
    return false;
  {
    StateLock lock(gState);
    if (lock.ok() && (gState.sos || gState.sosEscalated))
      return false;
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

  String p = "SOS,";
  p += valid ? String(lat, 6) + "," + String(lon, 6) : "NOFIX";
  p += ",TS=" + String(millis());
  uint8_t routed[Config::LORA_MAX_PACKET] = {};
  const uint16_t seq = ++sosSeq_;
  const size_t routedLen = addRouteExtension(
      reinterpret_cast<const uint8_t*>(p.c_str()), p.length(), 0, 0,
      routed, sizeof(routed));
  String packet;
  if (!routedLen ||
      !encryptPacket(routed, routedLen, Config::LORA_TYPE_SOS, seq, packet))
    return false;
  sosPacket_ = packet;
  sosSentMs_ = millis();
  sosRetryCount_ = 0;
  sosAwaitingAck_ = true;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.sosSeq = seq;
      gState.sosAcked = false;
      gState.sosEscalated = false;
      gState.sosRetries = 0;
      gState.sosStartedMs = millis();
      gState.sosAckedBy = 0;
      gState.sos = true;
    }
  }
  addSosHistory(0);
  return transmit(packet, true);
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
