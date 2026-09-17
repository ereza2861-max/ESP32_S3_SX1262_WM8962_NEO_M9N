#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class RadioOwner : uint8_t {
  None = 0,
  LoRaP2P = 1,
  LoRaWAN = 2,
};

class RadioArbiter {
public:
  RadioArbiter();
  bool acquire(RadioOwner owner, TickType_t timeout);
  void release(RadioOwner owner);
  RadioOwner currentOwner() const;
  bool isFree() const;

private:
  SemaphoreHandle_t mutex_ = nullptr;
  TaskHandle_t ownerTask_ = nullptr;
  RadioOwner owner_ = RadioOwner::None;
  uint16_t depth_ = 0;
};

class RadioArbiterGuard {
public:
  RadioArbiterGuard(RadioArbiter& arbiter, RadioOwner owner, TickType_t timeout)
      : arbiter_(arbiter), owner_(owner),
        locked_(arbiter_.acquire(owner_, timeout)) {}
  ~RadioArbiterGuard() {
    if (locked_) arbiter_.release(owner_);
  }
  bool ok() const { return locked_; }
  RadioArbiterGuard(const RadioArbiterGuard&) = delete;
  RadioArbiterGuard& operator=(const RadioArbiterGuard&) = delete;

private:
  RadioArbiter& arbiter_;
  RadioOwner owner_;
  bool locked_;
};

extern RadioArbiter radioArbiter;
