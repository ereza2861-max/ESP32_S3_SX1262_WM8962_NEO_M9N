#include <unity.h>
static float apply(float x, float a, float b, unsigned model) {
  if(model==0) return x; if(model==1) return x+a*x*x+b; if(model==2) return x+a*x*x+b*x*x*x; return x;
}
void test_calibration_models(){TEST_ASSERT_FLOAT_WITHIN(0.001f,2.0f,apply(2,0,0,0));TEST_ASSERT_FLOAT_WITHIN(0.001f,5.0f,apply(2,1,1,1));TEST_ASSERT_FLOAT_WITHIN(0.001f,10.0f,apply(2,1,1,2));}
void setup(){UNITY_BEGIN();RUN_TEST(test_calibration_models);UNITY_END();} void loop(){}
