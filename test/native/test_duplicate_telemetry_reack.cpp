#include <unity.h>
#include "MqttAckRouter.h"

void test_ack_routing_keeps_duplicate_sample_identity() {
  uint32_t firstSample = 0;
  uint32_t duplicateSample = 0;
  TEST_ASSERT_TRUE(MqttAckRouter::registerPending(51, 0x12345678U));
  TEST_ASSERT_TRUE(MqttAckRouter::takeSampleId(51, firstSample));
  TEST_ASSERT_EQUAL_UINT32(0x12345678U, firstSample);
  // A retransmission is a new MQTT packet but must preserve the app sample ID.
  TEST_ASSERT_TRUE(MqttAckRouter::registerPending(52, firstSample));
  TEST_ASSERT_TRUE(MqttAckRouter::takeSampleId(52, duplicateSample));
  TEST_ASSERT_EQUAL_UINT32(firstSample, duplicateSample);
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ack_routing_keeps_duplicate_sample_identity);
  UNITY_END();
}
void loop() {}
