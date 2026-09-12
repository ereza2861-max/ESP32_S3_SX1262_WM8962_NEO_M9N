#include "LoRaManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "PersistentConfig.h"
#include "AudioManager.h"
#include "Telemetry.h"
#include <esp_system.h>
#include <Preferences.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <time.h>
#include <esp_attr.h>

extern AudioManager audio;

namespace {
constexpr uint8_t PACKET_MAGIC = 0xF1;
constexpr size_t PACKET_HEADER_V1 = 1 + 1 + 1 + 2 + 4;
constexpr size_t PACKET_HEADER_V2 = PACKET_HEADER_V1 + 4 + 1;
constexpr size_t PACKET_HEADER_V3 = PACKET_HEADER_V2 + 1 + sizeof(uint32_t);
constexpr uint8_t LORA_PROTOCOL_VERSION_HOP = 3;
constexpr size_t PACKET_TAG = Config::LORA_TAG_BYTES;
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
  if (storedHighWater > 0xFFFFFF00UL) {
    prefs.end();
    return false;
  }
  const uint32_t newHighWater = storedHighWater + Config::LORA_TX_SEQUENCE_RESERVATION;
  if (prefs.putUInt("txseq_hi", newHighWater) != sizeof(uint32_t)) {
    prefs.end();
    return false;
  }
  prefs.end();

  txSequence_ = static_cast<uint16_t>(storedHighWater & 0xFFFFU);
  return true;
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
    slot->seenMs = now;
    slot->lastEpochSec = packetEpochSec;
    return false;
  }

  const int16_t delta = static_cast<int16_t>(seq - slot->highestSeq);
  if (delta > 0) {
    const uint8_t shift = static_cast<uint8_t>(min<int16_t>(delta, Config::LORA_REPLAY_WINDOW_BITS));
    slot->bitmap = shift >= 32 ? 1U : (slot->bitmap << shift) | 1U;
    slot->highestSeq = seq;
    slot->seenMs = now;
    slot->lastEpochSec = packetEpochSec;
    return false;
  }

  const int16_t age = static_cast<int16_t>(slot->highestSeq - seq);
  if (age >= Config::LORA_REPLAY_WINDOW_BITS) return true;
  const uint32_t bit = 1UL << age;
  if (slot->bitmap & bit) return true;
  slot->bitmap |= bit;
  slot->seenMs = now;
  slot->lastEpochSec = packetEpochSec;
  (void)payloadHash;
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

bool LoRaManager::transmitHopped(const String& text, uint8_t type) {
  if (!ready_ || !mutex_ || text.isEmpty()) return false;
  bool hopOn = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    hopOn = gState.hopEnabled && gState.hopChannelCount > 0;
  }
  if (text.length() > Config::LORA_MAX_PACKET - PACKET_HEADER_V3 - PACKET_TAG) return false;
  const uint8_t hopIndex = hopOn ? computeHopIndex(hopFrame_) : 0;
  const uint32_t epochSec = currentEpochSec();
  const uint16_t seq = ++txSequence_;
  String packet;
  if (!encryptPacketV3(reinterpret_cast<const uint8_t*>(text.c_str()), text.length(), type, seq, hopIndex, epochSec, packet)) return false;
  if (hopOn && !retuneToHopChannel(hopIndex)) { (void)retuneToChannel0(); return false; }
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) { (void)retuneToChannel0(); return false; }
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
  if (!txOk) return false;
  ++hopFrame_;
  if (type == Config::LORA_TYPE_TEXT) {
    textPendingPacket_ = packet;
    textPendingSeq_ = seq;
    textRetryCount_ = 0;
    textSentMs_ = millis();
    textAwaitingAck_ = true;
    textAcked_ = false;
  }
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
                                  uint8_t ttl, const uint8_t* payload, size_t len) {
  if (!forwardQueue_ || !payload || !len ||
      len > Config::LORA_MAX_PACKET || ttl <= 1) return false;
  if (!forwardRateAllowed(sourceId, type)) return false;

  ForwardPacket packet{};
  packet.type = type;
  packet.ttl = static_cast<uint8_t>(ttl - 1);
  packet.seq = seq;
  packet.sourceId = sourceId;
  packet.dedupId = hashPayload(payload, len) ^ sourceId ^
                   (static_cast<uint32_t>(seq) << 16);
  packet.receivedMs = millis();
  packet.len = static_cast<uint16_t>(len);
  memcpy(packet.payload, payload, len);
  return xQueueSend(forwardQueue_, &packet, 0) == pdPASS;
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
  uint8_t key[16];
  if (!loadKey(key)) return false;

  // Rebuild the authenticated v2 envelope while preserving the original
  // source ID and sequence. The nonce changes, so the forwarded ciphertext
  // cannot be replayed verbatim at the next hop.
  const uint32_t nonce = esp_random();
  packet.reserve(PACKET_HEADER_V2 + forward.len + PACKET_TAG);
  packet += static_cast<char>(PACKET_MAGIC);
  packet += static_cast<char>(Config::LORA_PROTOCOL_VERSION);
  packet += static_cast<char>(forward.type);
  packet += static_cast<char>(forward.seq & 0xFF);
  packet += static_cast<char>(forward.seq >> 8);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((nonce >> (8 * i)) & 0xFF);
  for (uint8_t i = 0; i < 4; ++i)
    packet += static_cast<char>((forward.sourceId >> (8 * i)) & 0xFF);
  packet += static_cast<char>(forward.ttl);

  uint8_t iv[16] = {};
  memcpy(iv, &nonce, sizeof(nonce));
  memcpy(iv + 4, &forward.seq, sizeof(forward.seq));
  uint8_t streamBlock[16] = {};
  uint8_t cipher[Config::LORA_MAX_PACKET] = {};
  size_t ncOff = 0;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
            mbedtls_aes_crypt_ctr(&aes, forward.len, &ncOff, iv, streamBlock,
                                  forward.payload, cipher) == 0;
  if (ok) {
    for (size_t i = 0; i < forward.len; ++i)
      packet += static_cast<char>(cipher[i]);
    unsigned char tag[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    ok = md && mbedtls_md_hmac(md, key, sizeof(key),
        reinterpret_cast<const unsigned char*>(packet.c_str()),
        PACKET_HEADER_V2 + forward.len, tag, sizeof(tag)) == 0;
    if (ok)
      for (size_t i = 0; i < PACKET_TAG; ++i)
        packet += static_cast<char>(tag[i]);
  }
  mbedtls_aes_free(&aes);
  if (!ok) return false;

  // Keep forwarding serialized with normal TX. The pending TX state machine
  // performs CAD/backoff and preserves this already-built forwarding envelope.
  if (Config::LORA_LBT_ENABLED) {
    if (pendingTx_.active || !queuePendingTx(packet)) return false;
    (void)processPendingTx();
    return true;
  }

  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
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
  return txOk;
}


bool LoRaManager::scannerStart(uint8_t mode, uint16_t dwellMs) {
  if (!ready_ || !mutex_ || (mode != 1 && mode != 2) ||
      dwellMs < Config::SCANNER_MIN_DWELL_MS ||
      dwellMs > Config::SCANNER_MAX_DWELL_MS)
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
  if (!results) return;
  const size_t n = min(static_cast<size_t>(Config::SCANNER_MAX_CHANNELS),
                       static_cast<size_t>(Config::HOP_CHANNEL_MAX));
  for (size_t i = 0; i < n; ++i) {
    results[i].freqMHz = scanner_.results[i].freqMHz;
    results[i].rssiAvgDbm = scanner_.results[i].rssiAvgDbm;
    results[i].rssiPeakDbm = scanner_.results[i].rssiPeakDbm;
    results[i].snrDb = scanner_.results[i].snrDb;
    results[i].occupancyPercent = scanner_.results[i].occupancyPercent;
    results[i].preambleCount = scanner_.results[i].preambleCount;
    results[i].timestamp = scanner_.results[i].timestamp;
  }
  count = n;
}

size_t LoRaManager::scannerSuggestBestChannels(uint8_t* channels, size_t capacity) {
  if (!channels || capacity == 0) return 0;
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
  const uint16_t seq = ++txSequence_;
  String packet;
  const uint32_t epochSec = currentEpochSec();
  if (!encryptPacketV3(payload, sizeof(payload), Config::LORA_TYPE_TEXT_ACK, seq, hopIndex, epochSec, packet)) return false;
  return transmit(packet, true);
}

void LoRaManager::handleTextAckPayload(const uint8_t* payload, size_t len) {
  if (!payload || len != 6) return;
  uint16_t ackedSeq = static_cast<uint16_t>(payload[0]) | (static_cast<uint16_t>(payload[1]) << 8);
  uint32_t ackedSource = 0;
  memcpy(&ackedSource, payload + 2, sizeof(ackedSource));
  if (ackedSource != sourceId_ || !textAwaitingAck_ || ackedSeq != textPendingSeq_) return;
  textAwaitingAck_ = false;
  textAcked_ = true;
}

void LoRaManager::serviceTextRetry() {
  if (!textAwaitingAck_ || textPendingPacket_.isEmpty() || millis() - textSentMs_ < Config::SOS_REPEAT_MS) return;
  if (textRetryCount_ >= Config::SOS_MAX_RETRIES) {
    textAwaitingAck_ = false;
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "Text ACK timeout";
    return;
  }
  if (transmit(textPendingPacket_, true)) {
    ++textRetryCount_;
    textSentMs_ = millis();
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
  String packet;
  const uint16_t seq = ++txSequence_;
  if (!encryptPacket(payload, sizeof(payload), 2, seq, packet))
    return false;
  return transmit(packet, true);
}

bool LoRaManager::begin() {
  instance_ = this;
  mutex_ = xSemaphoreCreateMutex();
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
  if (!mutex_ || !forwardQueue_ || !reserveTxSequenceBlock()) return false;

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
    const bool isForwardable = rxAuthenticated && (isV2 || isV3);
    const bool duplicateV2 = (isV2 || isV3) &&
        seenDedup(rxSourceId, seq, type, hashPayload(plain, plainLen), isV3 ? epochMs : 0);
    bool pttOrRecording = false;
    {
      StateLock stateLock(gState);
      if (stateLock.ok()) pttOrRecording = gState.ptt || gState.recording;
    }
    if (rxAuthenticated && type == Config::LORA_TYPE_TEXT) {
      textAckSeq_ = seq;
      textAckSourceId_ = rxSourceId;
      textAckHopIndex_ = isV3 ? hopIndex : 0;
      textAckPending_ = true;
    }
    if (rxAuthenticated && type == Config::LORA_TYPE_TEXT && plainLen > 0 && !duplicateV2) {
      char text[Config::LORA_MAX_PACKET + 1] = {};
      memcpy(text, plain, plainLen);
      text[plainLen] = '\0';
      addMessageHistory(rxSourceId, text);
    }
    if (rxAuthenticated && type == Config::LORA_TYPE_SOS && plainLen > 0 &&
        !duplicateV2) {
      char text[Config::LORA_MAX_PACKET + 1] = {};
      memcpy(text, plain, plainLen);
      text[plainLen] = '\0';
      addMessageHistory(rxSourceId, text);
      (void)sendSosAck(seq, rxSourceId);
    }
    if (rxAuthenticated && type == Config::LORA_TYPE_SOS_ACK && !duplicateV2) {
      handleSosAckPayload(plain, plainLen);
    }
    if (rxAuthenticated && type == Config::LORA_TYPE_TEXT_ACK) {
      handleTextAckPayload(plain, plainLen);
    }
    if (rxAuthenticated && type == Config::LORA_TYPE_VOICE && !duplicateV2 && plainLen == 168 &&
        plain[0] == 0x56 && plain[1] == 1 &&
        (static_cast<uint16_t>(plain[4]) | (static_cast<uint16_t>(plain[5]) << 8)) == seq &&
        plain[2] == Config::VOICE_FRAME_MS &&
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
            memcpy(freeSlot->data, plain, sizeof(freeSlot->data));
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
      serviceVoiceReorder();
    }

    if (isForwardable && !duplicateV2 && rxSourceId != sourceId_ &&
        rxTtl > 1 && plainLen > 0) {
      (void)enqueueForward(type, seq, rxSourceId, rxTtl, plain, plainLen);
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
  if (!isPttOrRecording() && !pendingTx_.active &&
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

bool LoRaManager::queuePendingTx(const String& packet) {
  if (!Config::LORA_LBT_ENABLED || packet.isEmpty() || !mutex_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  bool queued = false;
  if (!pendingTx_.active) {
    pendingTx_.packet = packet;
    pendingTx_.retries = 0;
    pendingTx_.nextAttemptMs = millis();
    pendingTx_.active = true;
    queued = true;
  }
  xSemaphoreGive(mutex_);
  return queued;
}

bool LoRaManager::processPendingTx() {
  if (!Config::LORA_LBT_ENABLED || !ready_ || !mutex_) return false;
  if (xSemaphoreTake(mutex_, 0) != pdTRUE) return false;
  if (!pendingTx_.active ||
      static_cast<int32_t>(millis() - pendingTx_.nextAttemptMs) < 0) {
    xSemaphoreGive(mutex_);
    return false;
  }

  bool done = false;
  bool txOk = false;
  int16_t st = RADIOLIB_ERR_NONE;
  int16_t rxSt = RADIOLIB_ERR_NONE;
  bool budgetConsumed = false;
  RadioLibTime_t airtimeUs = 0;
  String pendingError;

  {
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
          // CAD leaves the SX127x in standby; restore RX while waiting.
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

  if (!pendingError.isEmpty()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = pendingError;
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
  return done;
}

bool LoRaManager::transmit(const String& text, bool alreadyEncrypted) {
  if (!ready_ || !mutex_ || text.isEmpty() ||
      text.length() > (alreadyEncrypted
          ? Config::LORA_MAX_PACKET
          : Config::LORA_MAX_PACKET - PACKET_HEADER_V2 - PACKET_TAG))
    return false;
  if (Config::LORA_REQUIRE_ENCRYPTION && gConfig.loraKeyHex.length() != 32)
    return false;

  String packet;
  if (alreadyEncrypted) {
    packet = text;
  } else {
    const uint8_t packetType = Config::LORA_TYPE_TEXT;
    const uint16_t seq = ++txSequence_;
    if (!encryptPacket(reinterpret_cast<const uint8_t*>(text.c_str()), text.length(),
                       packetType, seq, packet)) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa encryption/key configuration failed";
      return false;
    }
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
  if (ok) logPacket(true, 0, static_cast<uint16_t>(txSequence_ - 1), sourceId_, 0, 0.0f, Config::LORA_INITIAL_TTL);
  xSemaphoreGive(mutex_);
  return ok;
}

void LoRaManager::prepareForDeepSleep() {
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
  if (!transmitHopped(text, Config::LORA_TYPE_TEXT)) return false;
  const uint32_t deadline = millis() + 1500;
  while (textAwaitingAck_ && static_cast<int32_t>(millis() - deadline) < 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!textAcked_) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "Text transmitted; ACK not received";
  }
  return true;
}

bool LoRaManager::sendVoiceFrame() {
  uint8_t captured[164] = {};
  uint8_t frame[168] = {};
  size_t len = 0;
  if (!audio.captureVoiceFrame(captured, sizeof(captured), len) || len != sizeof(captured))
    return false;

  // captureVoiceFrame owns the transport marker + duration + PCM payload.
  // Build the sequence/CRC envelope separately instead of shifting the payload
  // in-place; this prevents future header changes from corrupting audio bytes.
  memcpy(frame, captured, 4);
  const uint16_t seq = voiceSequence_++;
  frame[4] = static_cast<uint8_t>(seq & 0xFF);
  frame[5] = static_cast<uint8_t>(seq >> 8);
  memcpy(frame + 6, captured + 4, 160);
  const uint16_t crc = crc16(frame, 166);
  frame[166] = static_cast<uint8_t>(crc & 0xFF);
  frame[167] = static_cast<uint8_t>(crc >> 8);
  String packet;
  if (!encryptPacket(frame, sizeof(frame), Config::LORA_TYPE_VOICE, seq, packet)) return false;
  if (packet.length() > Config::LORA_MAX_PACKET) return false;
  const bool ok = transmit(packet, true);
  if (ok) {
    {
      StateLock lock(gState);
      if (lock.ok()) gState.voiceTxPackets++;
    }
    logPacket(true, Config::LORA_TYPE_VOICE, seq, sourceId_, 0, 0.0f, Config::LORA_INITIAL_TTL);
  }
  return ok;
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
  String packet;
  const uint16_t seq = ++sosSeq_;
  if (!encryptPacket(reinterpret_cast<const uint8_t*>(p.c_str()), p.length(),
                     Config::LORA_TYPE_SOS, seq, packet))
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
  return true;
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
