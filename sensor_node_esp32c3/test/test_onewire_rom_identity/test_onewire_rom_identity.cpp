#include <cassert>
#include <cstdint>
#include "SensorProtocol.h"

static void testRomIdentityContract() {
  const uint8_t rom[8] = {0x28,1,2,3,4,5,6,7};
  SensorProtocol::RomListNotification packet{};
  packet.entries[0].sensorId = 0x0100;
  for (size_t i = 0; i < 8; ++i) packet.entries[0].rom[i] = rom[i];
  packet.entryCount = 1;
  assert(packet.magic == SensorProtocol::ROM_LIST_MAGIC);
  assert(packet.entries[0].rom[0] == 0x28);
}

void setup() { testRomIdentityContract(); }
void loop() {}
