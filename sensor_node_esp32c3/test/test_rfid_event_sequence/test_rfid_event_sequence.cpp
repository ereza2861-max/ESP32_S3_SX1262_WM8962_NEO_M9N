#include <cassert>
#include <cstdint>
#include "SensorRegistry.h"

static void testRfidSequenceMonotonicAcrossRegistryRestart() {
  SensorRegistry firstBoot;
  const uint32_t first = firstBoot.nextRfidSequence();
  assert(first != 0);

  // UNIT_TEST mock NVS keeps the high-water mark across object recreation,
  // modelling the persistent sequence surviving a firmware restart.
  SensorRegistry afterReboot;
  const uint32_t second = afterReboot.nextRfidSequence();
  assert(second == first + 1U);
}

void setup() { testRfidSequenceMonotonicAcrossRegistryRestart(); }
void loop() {}
