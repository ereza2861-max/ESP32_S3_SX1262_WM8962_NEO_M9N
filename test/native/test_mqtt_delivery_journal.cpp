#include <unity.h>
void test_journal_states() {
  enum { PENDING=1, DELIVERED=2, PENDING_RETRY=3 };
  TEST_ASSERT_EQUAL(1,PENDING); TEST_ASSERT_EQUAL(2,DELIVERED); TEST_ASSERT_EQUAL(3,PENDING_RETRY);
}
void setup(){UNITY_BEGIN();RUN_TEST(test_journal_states);UNITY_END();} void loop(){}
