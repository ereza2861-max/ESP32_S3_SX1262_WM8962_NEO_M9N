#include "EncryptedFrameParser.h"
#include <cstring>

namespace EncryptedFrameParser {

bool parse(const uint8_t* packet, size_t length, uint8_t expectedVersion,
           size_t headerBytes, size_t tagBytes, Parsed& out,
           size_t maxPacketBytes) {
  out = {};
  if (!packet || headerBytes < 19 || headerBytes > 20 || tagBytes == 0 ||
      length < headerBytes + tagBytes || length > maxPacketBytes) return false;
  if (packet[0] != 0xF1 || packet[1] != expectedVersion) return false;

  out.type = packet[2];
  out.sequence = static_cast<uint16_t>(packet[3]) |
                 (static_cast<uint16_t>(packet[4]) << 8U);
  std::memcpy(&out.sourceId, packet + 9, sizeof(out.sourceId));
  out.ttl = packet[13];
  out.hopIndex = packet[14];
  std::memcpy(&out.epochSec, packet + 15, sizeof(out.epochSec));
  if (headerBytes == 20) out.keyEpochDelta = packet[19];

  out.cipherOffset = headerBytes;
  out.cipherLength = length - headerBytes - tagBytes;
  out.tagOffset = headerBytes + out.cipherLength;
  return out.tagOffset + tagBytes == length;
}

}  // namespace EncryptedFrameParser
