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
};
