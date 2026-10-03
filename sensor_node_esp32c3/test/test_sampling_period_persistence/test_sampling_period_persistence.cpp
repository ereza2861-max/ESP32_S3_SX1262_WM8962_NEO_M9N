#include <cassert>
#include <cstdint>

static bool persistThenApply(bool persistOk, uint32_t& runtime, uint32_t requested) {
  if (!persistOk) return false;
  runtime = requested;
  return true;
}

static void testSamplingPeriodIsFailClosed() {
  uint32_t runtime = 1000;
  assert(!persistThenApply(false, runtime, 5000));
  assert(runtime == 1000);
  assert(persistThenApply(true, runtime, 5000));
  assert(runtime == 5000);
}

void setup() { testSamplingPeriodIsFailClosed(); }
void loop() {}
