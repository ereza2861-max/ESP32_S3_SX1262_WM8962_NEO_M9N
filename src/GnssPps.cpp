#include "GnssPps.h"
#include "BoardConfig.h"
#include <esp_attr.h>

void IRAM_ATTR GnssPps::isr(void* arg) {
  auto* self = static_cast<GnssPps*>(arg);
  if (self) self->lastEdgeUs_ = micros();
}

bool GnssPps::begin(int pin) {
  if (pin < 0) return false;
  pin_ = pin;
  pinMode(pin_, INPUT);
  attachInterruptArg(pin_, isr, this, RISING);
  // TODO: validate the selected PPS GPIO against the final PCB routing.
  return true;
}
