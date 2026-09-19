#pragma once

#include <cstddef>
#include <cstdint>

namespace EncryptedFrameParser {

struct Parsed {
  uint8_t type = 0;
  uint16_t sequence = 0;
  uint32_t sourceId = 0;
  uint8_t ttl = 0;
  uint8_t hopIndex = 0;
  uint32_t epochSec = 0;
  uint8_t keyEpochDelta = 0;
  size_t cipherOffset = 0;
  size_t cipherLength = 0;
  size_t tagOffset = 0;
};

bool parse(const uint8_t* packet, size_t length, uint8_t expectedVersion,
           size_t headerBytes, size_t tagBytes, Parsed& out,
           size_t maxPacketBytes);

}  // namespace EncryptedFrameParser
