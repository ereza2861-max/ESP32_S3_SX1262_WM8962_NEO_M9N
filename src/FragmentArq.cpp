#include "FragmentArq.h"

bool FragmentArq::begin() {
  // Fragment transport is implemented by LoRaManager; this object is the
  // lifecycle marker retained for dependency-injection/tests.
  enabled_ = true;
  return true;
}

void FragmentArq::task() {
  // LoRaManager owns the radio/ARQ state machine. Keep this task non-blocking.
}
