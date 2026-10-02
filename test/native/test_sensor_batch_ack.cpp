#include <cassert>
#include <cstdint>
#include "SensorBatchAck.h"

int main() {
  SensorBatchAck::Record records[2]{};
  records[0].nodeId = 0x11223344UL;
  records[0].sensorId = 0x0102;
  records[0].baseSequence = 100;
  records[0].bitmap = 0x00000005UL;
  records[1].nodeId = 0x55667788UL;
  records[1].sensorId = 0x0304;
  records[1].baseSequence = 200;
  records[1].bitmap = 0x80000001UL;

  uint8_t wire[64]{};
  const size_t len = SensorBatchAck::encode(records, 2, wire, sizeof(wire));
  assert(len == 3U + 2U * SensorBatchAck::RECORD_BYTES);

  SensorBatchAck::Record decoded[2]{};
  size_t count = 0;
  assert(SensorBatchAck::decode(wire, len, decoded, 2, count));
  assert(count == 2);
  assert(decoded[0].nodeId == records[0].nodeId);
  assert(decoded[0].sensorId == records[0].sensorId);
  assert(decoded[0].baseSequence == records[0].baseSequence);
  assert(decoded[0].bitmap == records[0].bitmap);
  assert(decoded[1].nodeId == records[1].nodeId);
  assert(decoded[1].bitmap == records[1].bitmap);
  return 0;
}
