#include <unity.h>
#include "Config.h"
void test_ack_mram_region_is_after_existing_config() {
  TEST_ASSERT_TRUE(0x3040 > Config::PERSISTENT_CONFIG_MRAM_MARKER +
                   Config::PERSISTENT_CONFIG_MRAM_MARKER_BYTES);
  TEST_ASSERT_TRUE(0x3240 + 512 <= Config::MRAM_SIZE_BYTES);
}
void setup(){UNITY_BEGIN();RUN_TEST(test_ack_mram_region_is_after_existing_config);UNITY_END();} void loop(){}
