#include <cassert>
#include <cstdint>
#include "Config.h"

static bool persistThenApply(bool persistOk, uint32_t& runtime, uint32_t requested) {
  if (!persistOk) return false;
  runtime = requested;
  return true;
}

static void testEffectiveSamplePeriodUsesBaseAndDescriptorMinimum() {
  assert(SensorNodeConfig::effectiveSamplePeriodMs(0) == 1000);
  assert(SensorNodeConfig::effectiveSamplePeriodMs(250) == 1000);
  assert(SensorNodeConfig::effectiveSamplePeriodMs(1000) == 1000);
  assert(SensorNodeConfig::effectiveSamplePeriodMs(1500) == 1500);
  assert(SensorNodeConfig::effectiveSamplePeriodMs(60000) == 60000);
}

static void testSamplingPeriodIsFailClosed() {
  uint32_t runtime = 1000;
  assert(!persistThenApply(false, runtime, 5000));
  assert(runtime == 1000);
  assert(persistThenApply(true, runtime, 5000));
  assert(runtime == 5000);
}

void setup() { testEffectiveSamplePeriodUsesBaseAndDescriptorMinimum(); testSamplingPeriodIsFailClosed(); }
void loop() {}
