#include <unity.h>
#include "Config.h"

void test_mram_commit_order_contract() {
  TEST_ASSERT_EQUAL_HEX16(0x3000, Config::PERSISTENT_CONFIG_MRAM_COMMIT_A);
  TEST_ASSERT_EQUAL_HEX16(0x3001, Config::PERSISTENT_CONFIG_MRAM_COMMIT_B);
  TEST_ASSERT_EQUAL_HEX16(0x3010, Config::PERSISTENT_CONFIG_MRAM_MARKER);
  TEST_ASSERT_EQUAL_UINT8(0xA5, Config::PERSISTENT_CONFIG_MRAM_COMMIT);
  TEST_ASSERT_EQUAL_UINT16(16, Config::PERSISTENT_CONFIG_MRAM_MARKER_BYTES);
}

void test_mram_slots_are_disjoint() {
  TEST_ASSERT_TRUE(Config::PERSISTENT_CONFIG_MRAM_SLOT_A + Config::PERSISTENT_CONFIG_MRAM_SLOT_BYTES <=
                   Config::PERSISTENT_CONFIG_MRAM_SLOT_B);
  TEST_ASSERT_TRUE(Config::PERSISTENT_CONFIG_MRAM_SLOT_B + Config::PERSISTENT_CONFIG_MRAM_SLOT_BYTES <=
                   Config::PERSISTENT_CONFIG_MRAM_COMMIT_A);
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_mram_commit_order_contract);
  RUN_TEST(test_mram_slots_are_disjoint);
  UNITY_END();
}

void loop() {}
