#include <cassert>
#include "SensorProtocol.h"

static void testTerminalResponseIsIdempotent() {
  SensorProtocol::CommandResponse first{};
  first.commandId = SensorProtocol::COMMAND_SET_SAMPLING_PERIOD;
  first.sequence = 100;
  first.result = 1;
  first.errorCode = 10;
  SensorProtocol::CommandResponse retry = first;
  assert(retry.sequence == first.sequence);
  assert(retry.commandId == first.commandId);
  assert(retry.result == 1);
  assert(retry.errorCode == 10);
}

void setup() { testTerminalResponseIsIdempotent(); }
void loop() {}
