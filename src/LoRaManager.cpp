#include "LoRaManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "PersistentConfig.h"
#include "AudioManager.h"

extern AudioManager audio;

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
  if (spiOk && readSt == RADIOLIB_ERR_NONE && msg.length() == 164 &&
      static_cast<uint8_t>(msg[0]) == 0x56 && static_cast<uint8_t>(msg[1]) == 1) {
    if (audio.playVoiceFrame(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length())) {
      StateLock voiceLock(gState);
      if (voiceLock.ok()) gState.voiceRxPackets++;
    } else {
      StateLock voiceLock(gState);
      if (voiceLock.ok()) gState.voiceDrops++;
    }
  }
  StateLock lock(gState);
  if (lock.ok()) {
    if (!spiOk) {
      gState.rxDrops++;
    } else if (readSt == RADIOLIB_ERR_NONE) {
      gState.rxPackets++;
      gState.lastMessage = msg;
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

bool LoRaManager::transmitLocked(const String& text) {
  if (!ready_ || !mutex_ || text.isEmpty() || text.length() > Config::LORA_MAX_PACKET)
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

    const uint32_t txStartMs = millis();
    st = radio_.transmit(text);
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
  return transmitLocked(text);
}

bool LoRaManager::sendVoiceFrame() {
  uint8_t frame[192] = {};
  size_t len = 0;
  if (!audio.captureVoiceFrame(frame, sizeof(frame), len) || len == 0) return false;

  String packet;
  packet.reserve(len);
  for (size_t i = 0; i < len; ++i) packet += static_cast<char>(frame[i]);
  const bool ok = transmitLocked(packet);
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

  return transmitLocked("POS," + String(lat, 6) + "," + String(lon, 6) +
                        "," + String(alt, 1) + "," + String(sat));
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
  return transmitLocked(p);
}
