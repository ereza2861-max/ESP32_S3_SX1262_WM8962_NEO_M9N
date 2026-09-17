#include "RadioArbiter.h"

RadioArbiter radioArbiter;

RadioArbiter::RadioArbiter() {
  mutex_ = xSemaphoreCreateRecursiveMutex();
}

bool RadioArbiter::acquire(RadioOwner owner, TickType_t timeout) {
  if (owner == RadioOwner::None || !mutex_) return false;
  if (xSemaphoreTakeRecursive(mutex_, timeout) != pdTRUE) return false;

  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  if (depth_ == 0) {
    ownerTask_ = task;
    owner_ = owner;
  } else if (ownerTask_ != task || owner_ != owner) {
    xSemaphoreGiveRecursive(mutex_);
    return false;
  }
  ++depth_;
  return true;
}

void RadioArbiter::release(RadioOwner owner) {
  if (!mutex_ || owner == RadioOwner::None ||
      owner_ != owner || ownerTask_ != xTaskGetCurrentTaskHandle() ||
      depth_ == 0)
    return;

  --depth_;
  if (depth_ == 0) {
    owner_ = RadioOwner::None;
    ownerTask_ = nullptr;
  }
  xSemaphoreGiveRecursive(mutex_);
}

RadioOwner RadioArbiter::currentOwner() const {
  return owner_;
}

bool RadioArbiter::isFree() const {
  return owner_ == RadioOwner::None;
}
