#include "LoRaManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "PersistentConfig.h"
#include "AudioManager.h"
#include "Telemetry.h"
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

extern AudioManager audio;

namespace {
constexpr uint8_t PACKET_MAGIC = 0xF1;
constexpr size_t PACKET_HEADER = 1 + 1 + 1 + 2 + 4;
constexpr size_t PACKET_TAG = Config::LORA_TAG_BYTES;

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
    : module_(Board::LORA_CS, Board::LORA_DIO0, Board::LORA_RST, Board::LORA_DIO1),
      radio_(&module_) {}

void LoRaManager::onDio0() {
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
  if (!plain || !loadKey(key) || len + PACKET_HEADER + PACKET_TAG > Config::LORA_MAX_PACKET)
    return false;

  uint32_t nonce = esp_random();
  packet.reserve(PACKET_HEADER + len + PACKET_TAG);
  packet += static_cast<char>(PACKET_MAGIC);
  packet += static_cast<char>(Config::LORA_PROTOCOL_VERSION);
  packet += static_cast<char>(type);
  packet += static_cast<char>(seq & 0xFF);
  packet += static_cast<char>(seq >> 8);
  for (uint8_t i = 0; i < 4; ++i) packet += static_cast<char>((nonce >> (8 * i)) & 0xFF);

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
    for (size_t i = 0; i < len; ++i) packet += static_cast<char>(cipher[i]);
    unsigned char tag[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    ok = md && mbedtls_md_hmac(md, key, sizeof(key),
                               reinterpret_cast<const unsigned char*>(packet.c_str()),
                               PACKET_HEADER + len, tag, sizeof(tag)) == 0;
    if (ok) for (size_t i = 0; i < PACKET_TAG; ++i) packet += static_cast<char>(tag[i]);
  }
  mbedtls_aes_free(&aes);
  return ok;
}

bool LoRaManager::decryptPacket(const String& packet, uint8_t& type, uint16_t& seq,
                                uint8_t* plain, size_t capacity, size_t& len) {
  len = 0;
  if (packet.length() < PACKET_HEADER + PACKET_TAG ||
      static_cast<uint8_t>(packet[0]) != PACKET_MAGIC ||
      static_cast<uint8_t>(packet[1]) != Config::LORA_PROTOCOL_VERSION)
    return false;
  const size_t cipherLen = packet.length() - PACKET_HEADER - PACKET_TAG;
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
      PACKET_HEADER + cipherLen, expected, sizeof(expected)) != 0) return false;

  const uint8_t* got = reinterpret_cast<const uint8_t*>(packet.c_str()) + PACKET_HEADER + cipherLen;
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
          reinterpret_cast<const unsigned char*>(packet.c_str()) + PACKET_HEADER, plain) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) return false;
  len = cipherLen;
  return true;
}

bool LoRaManager::begin() {
  instance_ = this;
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;

  SpiLock spiLock(pdMS_TO_TICKS(1000));
  if (!spiLock.ok()) return false;

  int16_t st = radio_.begin(
      gConfig.loraFreqMHz, gConfig.loraBwKHz, gConfig.loraSf,
      gConfig.loraCr, gConfig.loraSyncWord, gConfig.loraPowerDbm,
      Config::LORA_PREAMBLE, 0);

  if (st != RADIOLIB_ERR_NONE) {
    ready_ = false;
    StateLock lock(gState);
    if (lock.ok()) {
      gState.loraReady = false;
      gState.lastError = "SX1276 init failed: " + String(st);
    }
    return false;
  }

  radio_.setPacketReceivedAction(onDio0);
  st = radio_.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    ready_ = false;
    StateLock lock(gState);
    if (lock.ok()) {
      gState.loraReady = false;
      gState.lastError = "SX1276 RX failed: " + String(st);
    }
    return false;
  }

  ready_ = true;
  StateLock lock(gState);
  if (lock.ok()) gState.loraReady = true;
  return true;
}

void LoRaManager::task() {
  if (!mutex_) return;

  bool ptt = false;
  {
    StateLock lock(gState);
    if (lock.ok()) ptt = gState.ptt;
  }
  if (ptt && millis() - lastVoiceTxMs_ >= Config::VOICE_FRAME_MS) {
    lastVoiceTxMs_ = millis();
    (void)sendVoiceFrame();
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
            Config::LORA_PREAMBLE, 0);
        if (beginSt == RADIOLIB_ERR_NONE) {
          radio_.setPacketReceivedAction(onDio0);
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
    return;
  }

  bool spiOk = false;
  int16_t readSt = RADIOLIB_ERR_NONE;
  int16_t rxSt = RADIOLIB_ERR_NONE;
  String msg;

  {
    SpiLock spiLock(pdMS_TO_TICKS(20));
    if (spiLock.ok()) {
      spiOk = true;
      readSt = radio_.readData(msg);
      rxSt = radio_.startReceive();
    }
  }

  // Never hold the SPI mutex while taking the global state mutex. Other
  // managers update state after releasing SPI, so this lock ordering avoids
  // a cross-task deadlock.
  if (spiOk && readSt == RADIOLIB_ERR_NONE) {
    const int16_t rssi = static_cast<int16_t>(radio_.getRSSI());
    const float snr = radio_.getSNR();
    uint8_t plain[220] = {};
    uint8_t type = 0;
    uint16_t seq = 0;
    size_t plainLen = 0;
    const bool authenticated = decryptPacket(msg, type, seq, plain, sizeof(plain), plainLen);
    bool pttOrRecording = false;
    {
      StateLock stateLock(gState);
      if (stateLock.ok()) pttOrRecording = gState.ptt || gState.recording;
    }
    if (authenticated && type == 0 && plainLen > 0) {
      StateLock textLock(gState);
      if (textLock.ok()) gState.lastMessage = String(reinterpret_cast<const char*>(plain)).substring(0, plainLen);
    }
    if (authenticated && type == 1 && plainLen == 166 &&
        plain[0] == 0x56 && plain[1] == 1 &&
        (static_cast<uint16_t>(plain[4]) | (static_cast<uint16_t>(plain[5]) << 8)) == seq &&
        plain[2] == Config::VOICE_FRAME_MS &&
        rssi >= Config::VOICE_RSSI_THRESHOLD_DBM &&
        snr >= Config::VOICE_SNR_THRESHOLD_DB && !pttOrRecording) {
      bool duplicate = haveVoiceRxSequence_ && seq == lastVoiceRxSequence_;
      if (!duplicate) {
        if (haveVoiceRxSequence_) {
          const uint16_t expected = static_cast<uint16_t>(lastVoiceRxSequence_ + 1);
          if (seq != expected) {
            StateLock lossLock(gState);
            if (lossLock.ok()) gState.voiceRxLost += static_cast<uint16_t>(seq - expected);
          }
        }
        lastVoiceRxSequence_ = seq;
        haveVoiceRxSequence_ = true;
        if (audio.playVoiceFrame(plain, plainLen)) {
          StateLock voiceLock(gState);
          if (voiceLock.ok()) {
            gState.voiceRxPackets++;
            gState.rxActive = true;
            gState.rxActivityMs = millis();
          }
        } else {
          StateLock voiceLock(gState);
          if (voiceLock.ok()) gState.voiceDrops++;
        }
      }
    }
    StateLock rxState(gState);
    if (rxState.ok()) {
      gState.loraRssi = rssi;
      gState.loraSnr = snr;
    }
  }
  StateLock lock(gState);
  if (lock.ok()) {
    if (!spiOk) {
      gState.rxDrops++;
    } else if (readSt == RADIOLIB_ERR_NONE) {
      gState.rxPackets++;
      if (!authenticated) gState.lastMessage = "RX: authentication failed";
    } else {
      gState.rxDrops++;
    }

    if (spiOk && rxSt != RADIOLIB_ERR_NONE) {
      gState.rxDrops++;
      ready_ = false;
      gState.loraReady = false;
      gState.lastError = "SX1276 RX restart failed: " + String(rxSt);
    }
  }
  xSemaphoreGive(mutex_);
}

bool LoRaManager::transmit(const String& text, bool alreadyEncrypted) {
  if (!ready_ || !mutex_ || text.isEmpty() ||
      text.length() > Config::LORA_MAX_PACKET - PACKET_HEADER - PACKET_TAG)
    return false;
  if (Config::LORA_REQUIRE_ENCRYPTION && gConfig.loraKeyHex.length() != 32)
    return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  int16_t st = -1;
  int16_t rxSt = -1;
  {
    SpiLock spiLock(pdMS_TO_TICKS(1000));
    if (!spiLock.ok()) { xSemaphoreGive(mutex_); return false; }

    const RadioLibTime_t airtimeUs = radio_.getTimeOnAir(text.length());
    if (airtimeUs == 0 || airtimeUs > UINT32_MAX ||
        !consumeDutyBudget(static_cast<uint32_t>(airtimeUs))) {
      xSemaphoreGive(mutex_);
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa duty-cycle budget exhausted";
      return false;
    }

    String packet;
    if (alreadyEncrypted) {
      packet = text;
    } else {
      const uint8_t packetType = 0;
      const uint16_t seq = ++txSequence_;
      if (!encryptPacket(reinterpret_cast<const uint8_t*>(text.c_str()), text.length(),
                         packetType, seq, packet)) {
      xSemaphoreGive(mutex_);
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "LoRa encryption/key configuration failed";
        return false;
      }
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
        gState.lastError = "SX1276 RX restart failed: " + String(rxSt);
      }
      if (ok) gState.txPackets++;
    }
  }
  xSemaphoreGive(mutex_);
  return ok;
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
          Config::LORA_PREAMBLE, 0);
      if (st == RADIOLIB_ERR_NONE) {
        radio_.setPacketReceivedAction(onDio0);
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
  return transmit(text);
}

bool LoRaManager::sendVoiceFrame() {
  uint8_t frame[192] = {};
  size_t len = 0;
  if (!audio.captureVoiceFrame(frame, sizeof(frame), len) || len != 164) return false;
  memmove(frame + 6, frame + 4, 160);
  const uint16_t seq = voiceSequence_++;
  frame[4] = static_cast<uint8_t>(seq & 0xFF);
  frame[5] = static_cast<uint8_t>(seq >> 8);
  uint16_t crc = crc16(frame, 166 - 2);
  frame[164] = static_cast<uint8_t>(crc & 0xFF);
  frame[165] = static_cast<uint8_t>(crc >> 8);
  String packet;
  if (!encryptPacket(frame, 166, 1, seq, packet)) return false;
  if (packet.length() > Config::LORA_MAX_PACKET) return false;
  const bool ok = transmit(packet, true);
  if (ok) {
    StateLock lock(gState);
    if (lock.ok()) gState.voiceTxPackets++;
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
  return transmit(p);
}
