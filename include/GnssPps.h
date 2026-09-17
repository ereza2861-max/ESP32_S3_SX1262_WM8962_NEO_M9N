#pragma once
#include <Arduino.h>

class GnssPps {
public:
  bool begin(int pin);
  uint32_t lastEdgeUs() const { return lastEdgeUs_; }

private:
  int pin_ = -1;
  volatile uint32_t lastEdgeUs_ = 0;
  static void IRAM_ATTR isr(void* arg);
};
