#include <unity.h>
#include "MqttAckRouter.h"

void test_interleaved_packet_does_not_consume_expected_puback() {
  uint32_t sampleId = 0;
  TEST_ASSERT_TRUE(MqttAckRouter::registerPending(42, 9001));
  TEST_ASSERT_TRUE(MqttAckRouter::registerPending(7, 7001));
  TEST_ASSERT_FALSE(MqttAckRouter::takeSampleId(8, sampleId));
  TEST_ASSERT_EQUAL_UINT32(0, sampleId);
  TEST_ASSERT_TRUE(MqttAckRouter::takeSampleId(42, sampleId));
  TEST_ASSERT_EQUAL_UINT32(9001, sampleId);
  TEST_ASSERT_TRUE(MqttAckRouter::takeSampleId(7, sampleId));
  TEST_ASSERT_EQUAL_UINT32(7001, sampleId);
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_interleaved_packet_does_not_consume_expected_puback);
  UNITY_END();
}
void loop() {}
