#include <cassert>
#include "SensorRegistry.h"

int main() {
  SensorRegistry registry(2, 1);
  SensorProtocol::BleAddress identity{{1,2,3,4,5,6}, 0};
  SensorProtocol::BleAddress rpa1{{6,5,4,3,2,1}, 1};
  SensorProtocol::BleAddress rpa2{{7,5,4,3,2,1}, 1};
  size_t index = 99;

  assert(registry.upsertNode(identity, "sensor-1", -40, 1, index));
  assert(index == 0);
  assert(registry.setLastRpa(index, rpa1));
  assert(registry.findNode(rpa1) == 0);

  // Simulate an RPA rotation: update the same identity record, never create
  // a second node for the new RPA.
  assert(registry.setLastRpa(index, rpa2));
  assert(registry.findNode(identity) == 0);
  assert(registry.findNode(rpa2) == 0);
  assert(registry.findNode(rpa1) < 0);

  SensorProtocol::BleAddress unknown{{9,9,9,9,9,9}, 1};
  assert(registry.findNode(unknown) < 0);
  size_t second = 0;
  assert(!registry.upsertNode(unknown, "unexpected-rpa", -80, 2, second));
  return 0;
}
