#include "MqttAckRouter.h"

#include <freertos/semphr.h>

SemaphoreHandle_t MqttAckRouter::mutex_ = nullptr;
MqttAckRouter::Pending MqttAckRouter::pending_[MqttAckRouter::MAX_PENDING]{};
MqttAckRouter::Callback MqttAckRouter::callback_ = nullptr;
void* MqttAckRouter::callbackCtx_ = nullptr;

bool MqttAckRouter::ensureMutex() {
  if (mutex_) return true;
  static portMUX_TYPE initMux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&initMux);
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  const bool ok = mutex_ != nullptr;
  portEXIT_CRITICAL(&initMux);
  return ok;
}

void MqttAckRouter::setCallback(Callback cb, void* ctx) {
  if (!ensureMutex()) return;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return;
  callback_ = cb;
  callbackCtx_ = ctx;
  xSemaphoreGive(mutex_);
}

bool MqttAckRouter::registerPending(uint16_t packetId, uint32_t sampleId) {
  if (packetId == 0 || sampleId == 0 || !ensureMutex()) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return false;

  for (auto& entry : pending_) {
    if (entry.valid && entry.packetId == packetId) {
      entry.sampleId = sampleId;
      xSemaphoreGive(mutex_);
      return true;
    }
  }
  for (auto& entry : pending_) {
    if (!entry.valid) {
      entry.packetId = packetId;
      entry.sampleId = sampleId;
      entry.valid = true;
      xSemaphoreGive(mutex_);
      return true;
    }
  }
  xSemaphoreGive(mutex_);
  return false;
}

void MqttAckRouter::forgetPending(uint16_t packetId) {
  if (packetId == 0 || !ensureMutex()) return;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return;
  for (auto& entry : pending_) {
    if (entry.valid && entry.packetId == packetId) {
      entry = {};
      break;
    }
  }
  xSemaphoreGive(mutex_);
}

bool MqttAckRouter::takeSampleId(uint16_t packetId, uint32_t& sampleId) {
  sampleId = 0;
  if (packetId == 0 || !ensureMutex()) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  bool found = false;
  for (auto& entry : pending_) {
    if (entry.valid && entry.packetId == packetId) {
      sampleId = entry.sampleId;
      entry = {};
      found = true;
      break;
    }
  }
  xSemaphoreGive(mutex_);
  return found;
}

bool MqttAckRouter::hasPending(uint16_t packetId) {
  if (!ensureMutex()) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) != pdTRUE) return false;
  bool found = false;
  for (const auto& item : pending_)
    if (item.valid && item.packetId == packetId) { found = true; break; }
  xSemaphoreGive(mutex_);
  return found;
}

void MqttAckRouter::onPublishSuccess(uint16_t packetId) {
  Callback cb = nullptr;
  void* ctx = nullptr;
  if (ensureMutex() && xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    cb = callback_;
    ctx = callbackCtx_;
    xSemaphoreGive(mutex_);
  }
  if (cb) cb(packetId, ctx);
}
