#include <unity.h>
void test_pending_reservation_is_not_a_commit() {
  bool committed=false, pending=true;
  TEST_ASSERT_TRUE(pending); TEST_ASSERT_FALSE(committed);
  committed=true; pending=false;
  TEST_ASSERT_TRUE(committed); TEST_ASSERT_FALSE(pending);
}
void setup(){UNITY_BEGIN();RUN_TEST(test_pending_reservation_is_not_a_commit);UNITY_END();} void loop(){}
