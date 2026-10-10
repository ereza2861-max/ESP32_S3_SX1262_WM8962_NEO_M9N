#include <unity.h>
#include "SensorTelemetry.h"
#include <cstring>

void test_timestamp_byte_20_is_covered_by_v2_crc() {
  uint8_t payload[SensorTelemetry::PAYLOAD_BYTES]{};
  SensorTelemetry::Decoded decoded{};
  TEST_ASSERT_EQUAL_UINT(SensorTelemetry::PAYLOAD_BYTES,
      SensorTelemetry::serializeSensorTelemetry(payload, 1, 2, 12.5f, 0,
                                               0x1122334455667788ULL));
  TEST_ASSERT_TRUE(SensorTelemetry::deserializeSensorTelemetry(
      payload, sizeof(payload), decoded));
  payload[20] ^= 0x01;
  TEST_ASSERT_FALSE(SensorTelemetry::deserializeSensorTelemetry(
      payload, sizeof(payload), decoded));
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_timestamp_byte_20_is_covered_by_v2_crc);
  UNITY_END();
}
void loop() {}
