#pragma once

#include <cstddef>
#include <cstdint>

namespace WebUiNumericParser {
inline bool parseUnsigned(const char* raw, std::size_t length,
                          uint32_t maxValue, uint32_t& out) {
  if (!raw || length == 0 || length > 10) return false;
  uint32_t value = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if (c < static_cast<unsigned char>('0') ||
        c > static_cast<unsigned char>('9'))
      return false;
    const uint32_t digit = static_cast<uint32_t>(c - static_cast<unsigned char>('0'));
    if (digit > maxValue || value > (maxValue - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  out = value;
  return true;
}
}  // namespace WebUiNumericParser
