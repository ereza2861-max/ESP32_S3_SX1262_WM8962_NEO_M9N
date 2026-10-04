#include <cassert>
#include <cstdint>
#include <cstring>
#include "SensorTelemetry.h"

namespace {
void test_wire_origin_is_preserved() {
  uint8_t payload[SensorTelemetry::PAYLOAD_BYTES] = {};
  const size_t n = SensorTelemetry::serializeSensorTelemetry(
      payload, 100U, 7U, 12.5f, 0U, 123456U, 0xABCDEF01U, 2U,
      0x01020304U, 300U, 200U);
  assert(n == SensorTelemetry::PAYLOAD_BYTES);

  SensorTelemetry::Decoded decoded{};
  assert(SensorTelemetry::deserializeSensorTelemetry(payload, n, decoded));
  assert(decoded.nodeId == 100U);
  assert(decoded.sensorId == 7U);
  assert(decoded.sourceSequence == 300U);
  assert(decoded.originNodeId == 200U);
}

void test_direct_remote_origin_is_source_identity() {
  uint8_t payload[SensorTelemetry::PAYLOAD_BYTES] = {};
  assert(SensorTelemetry::serializeSensorTelemetry(
             payload, 100U, 8U, 1.0f, 0U, 123457U, 0x55U, 1U,
             0x01020304U, 301U, 100U) == SensorTelemetry::PAYLOAD_BYTES);
  SensorTelemetry::Decoded decoded{};
  assert(SensorTelemetry::deserializeSensorTelemetry(
      payload, SensorTelemetry::PAYLOAD_BYTES, decoded));
  assert(decoded.originNodeId == decoded.nodeId);
}
}  // namespace

int main() {
  test_wire_origin_is_preserved();
  test_direct_remote_origin_is_source_identity();
  return 0;
}
