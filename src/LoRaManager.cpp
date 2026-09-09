#include "LoRaManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"

LoRaManager* LoRaManager::instance_ = nullptr;

LoRaManager::LoRaManager()
    : module_(Board::LORA_CS, Board::LORA_DIO0, Board::LORA_RST, Board::LORA_DIO1),
      radio_(&module_) {}

void LoRaManager::onDio0() {
  if (instance_) instance_->irqFlag_ = true;
}

bool LoRaManager::begin() {
  instance_ = this;
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;

  int16_t st = radio_.begin(
      Config::LORA_FREQ_MHZ, Config::LORA_BW_KHZ, Config::LORA_SF,
      Config::LORA_CR, Config::LORA_SYNC_WORD, Config::LORA_POWER_DBM,
      Config::LORA_PREAMBLE, 0);

  if (st != RADIOLIB_ERR_NONE) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "SX1276 init failed: " + String(st);
    return false;
  }

  radio_.setPacketReceivedAction(onDio0);
  st = radio_.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "SX1276 RX failed: " + String(st);
    return false;
  }

  ready_ = true;
  StateLock lock(gState);
  if (lock.ok()) gState.loraReady = true;
  return true;
}

void LoRaManager::task() {
  if (!ready_ || !irqFlag_) return;
  irqFlag_ = false;
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;

  String msg;
  int16_t st = radio_.readData(msg);
  if (st == RADIOLIB_ERR_NONE) {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.rxPackets++;
      gState.lastMessage = msg;
    }
  } else {
    StateLock lock(gState);
    if (lock.ok()) gState.rxDrops++;
  }
  radio_.startReceive();
  xSemaphoreGive(mutex_);
}

bool LoRaManager::transmitLocked(const String& text) {
  if (!ready_ || !mutex_ || text.isEmpty() || text.length() > Config::LORA_MAX_PACKET)
    return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

  int16_t st = radio_.transmit(text);
  radio_.startReceive();

  const bool ok = (st == RADIOLIB_ERR_NONE);
  if (ok) {
    StateLock lock(gState);
    if (lock.ok()) gState.txPackets++;
  }
  xSemaphoreGive(mutex_);
  return ok;
}

bool LoRaManager::sendText(const String& text) {
  return transmitLocked(text);
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
