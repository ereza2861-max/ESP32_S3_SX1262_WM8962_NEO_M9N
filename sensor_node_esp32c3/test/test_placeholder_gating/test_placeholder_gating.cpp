#include <cassert>
#include "SensorProtocol.h"

static void testDisabledDescriptorIsNeverTelemetryEnabled() {
  SensorProtocol::SensorDescriptor d{};
  d.id = 0x1234;
  d.flags = SensorProtocol::FLAG_DEGRADED;
  assert(!SensorProtocol::isEnabled(d));
  d.flags |= SensorProtocol::FLAG_ENABLED;
  assert(SensorProtocol::isEnabled(d));
}

void setup() {
  testDisabledDescriptorIsNeverTelemetryEnabled();
}
void loop() {}
