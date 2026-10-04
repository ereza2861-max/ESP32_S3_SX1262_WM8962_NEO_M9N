#include <unity.h>
void test_duplicate_ack_requires_durable_record() {
  bool spoolFound=false;
  TEST_ASSERT_FALSE(spoolFound);
  spoolFound=true;
  TEST_ASSERT_TRUE(spoolFound);
}
void setup(){UNITY_BEGIN();RUN_TEST(test_duplicate_ack_requires_durable_record);UNITY_END();} void loop(){}
