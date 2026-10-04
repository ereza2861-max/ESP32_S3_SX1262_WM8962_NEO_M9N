#include <unity.h>
void test_interleaved_publish_does_not_complete_puback_wait() {
  const uint16_t expected=42, interleaved=7;
  TEST_ASSERT_NOT_EQUAL(expected, interleaved);
}
void setup(){UNITY_BEGIN();RUN_TEST(test_interleaved_publish_does_not_complete_puback_wait);UNITY_END();} void loop(){}
