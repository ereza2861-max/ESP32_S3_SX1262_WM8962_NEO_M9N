#include <unity.h>
#include "Config.h"
void test_ack_retry_contract() {
  TEST_ASSERT_EQUAL_UINT8(5, Config::SENSOR_BATCH_ACK_MAX_RETRIES);
  TEST_ASSERT_TRUE(Config::SENSOR_BATCH_ACK_RETRY_BACKOFF_MS > 0);
}
void setup(){UNITY_BEGIN();RUN_TEST(test_ack_retry_contract);UNITY_END();} void loop(){}
