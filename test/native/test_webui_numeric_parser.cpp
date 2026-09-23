#include <cassert>
#include <cstdint>
#include <string>
#include "WebUiNumericParser.h"

int main() {
  uint32_t value = 0;

  assert(!WebUiNumericParser::parseUnsigned("", 0, 100, value));
  const char embeddedNull[] = {'1', '2', '\0', '3'};
  assert(!WebUiNumericParser::parseUnsigned(embeddedNull, sizeof(embeddedNull), 9999, value));

  const std::string spaced = " 12";
  assert(!WebUiNumericParser::parseUnsigned(spaced.data(), spaced.size(), 100, value));
  const std::string plus = "+12";
  const std::string minus = "-1";
  assert(!WebUiNumericParser::parseUnsigned(plus.data(), plus.size(), 100, value));
  assert(!WebUiNumericParser::parseUnsigned(minus.data(), minus.size(), 100, value));

  const std::string overflow = "42949672960";
  assert(!WebUiNumericParser::parseUnsigned(overflow.data(), overflow.size(), UINT32_MAX, value));

  const std::string trailing = "123x";
  assert(!WebUiNumericParser::parseUnsigned(trailing.data(), trailing.size(), 1000, value));

  const std::string valid = "429";
  assert(WebUiNumericParser::parseUnsigned(valid.data(), valid.size(), 500, value));
  assert(value == 429U);

  const std::string max = "500";
  assert(WebUiNumericParser::parseUnsigned(max.data(), max.size(), 500, value));
  assert(value == 500U);

  const std::string overMax = "501";
  assert(!WebUiNumericParser::parseUnsigned(overMax.data(), overMax.size(), 500, value));

  const std::string one = "1";
  assert(!WebUiNumericParser::parseUnsigned(one.data(), one.size(), 0, value));

  return 0;
}
