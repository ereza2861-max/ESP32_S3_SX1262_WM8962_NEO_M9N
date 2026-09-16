#pragma once

#include <Arduino.h>
#include "Config.h"

class RfDetector {
public:
  bool begin();
  bool read(float& forwardDbm, float& reflectedDbm, float& vswr);
  bool healthy() const { return healthy_; }
  float lastForwardDbm() const { return forwardDbm_; }
  float lastReflectedDbm() const { return reflectedDbm_; }
  float lastVswr() const { return vswr_; }

private:
  bool healthy_ = false;
  bool haveSample_ = false;
  float forwardDbm_ = -127.0f;
  float reflectedDbm_ = -127.0f;
  float vswr_ = Config::MAX2016_VSWR_MAX;

  static float voltageToDbm(uint32_t millivolts);
  static float returnLossToVswr(float returnLossDb);
};
