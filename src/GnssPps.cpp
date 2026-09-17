#include "GnssPps.h"
#include "BoardConfig.h"
#include <esp_attr.h>

void IRAM_ATTR GnssPps::isr(void* arg) {
  auto* self = static_cast<GnssPps*>(arg);
  if (self) self->lastEdgeUs_ = micros();
}

bool GnssPps::begin(int pin) {
  if (pin < 0 || pin > 48) return false;
  if (pin != Board::GNSS_PPS) return false;
  pin_ = pin;
  lastEdgeUs_ = 0;
  pinMode(pin_, INPUT);
  attachInterruptArg(pin_, isr, this, RISING);
  return true;
}
