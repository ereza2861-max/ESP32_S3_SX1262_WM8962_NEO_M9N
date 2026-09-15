#pragma once
#include <Arduino.h>
#include <TinyGPSPlus.h>

class GnssManager {
public:
  bool begin();
  void task();
private:
  TinyGPSPlus gps_;
  HardwareSerial serial_{1};
  uint64_t lastSyncEpoch_ = 0;
  uint32_t lastSyncMs_ = 0;
};
