#include <cassert>
#include <cstdint>
#include <random>
#include <vector>
#include "EncryptedFrameParser.h"

int main() {
  std::mt19937 rng(0xF1E1D1U);
  std::uniform_int_distribution<size_t> lengthDist(0, 220);
  std::uniform_int_distribution<uint32_t> byteDist(0, 255);

  for (size_t iteration = 0; iteration < 10000; ++iteration) {
    const size_t length = lengthDist(rng);
    std::vector<uint8_t> frame(length);
    for (auto& byte : frame) byte = static_cast<uint8_t>(byteDist(rng));

    EncryptedFrameParser::Parsed parsed{};
    const bool v3 = EncryptedFrameParser::parse(
        frame.data(), frame.size(), 3, 19, 16, parsed, 220);
    const bool v5 = EncryptedFrameParser::parse(
        frame.data(), frame.size(), 5, 20, 16, parsed, 220);
    if (v3 || v5) {
      assert(parsed.cipherOffset + parsed.cipherLength <= frame.size());
      assert(parsed.tagOffset + 16 == frame.size());
    }
  }
  return 0;
}
