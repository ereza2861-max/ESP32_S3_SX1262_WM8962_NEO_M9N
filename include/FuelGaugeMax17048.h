#pragma once
#include <Arduino.h>

class FuelGaugeMax17048 {
public:
  bool begin();
  bool available() const { return available_; }
  float voltage() const { return voltage_; }
  int8_t percent() const { return percent_; }
  void task();

private:
  bool available_ = false;
  float voltage_ = NAN;
  int8_t percent_ = -1;
  uint32_t lastPollMs_ = 0;
};
