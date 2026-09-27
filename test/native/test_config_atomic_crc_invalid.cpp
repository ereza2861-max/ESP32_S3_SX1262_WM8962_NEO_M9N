#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace {
constexpr uint32_t MAGIC = 0x43464732UL;
constexpr uint16_t SCHEMA = 3;
constexpr uint8_t COMMIT = 0xA5;

#pragma pack(push, 1)
struct Payload {
  uint8_t volume;
  uint16_t mqttPort;
  uint8_t ecdhPolicy;
};
struct Record {
  uint32_t magic;
  uint16_t schema;
  uint16_t payloadSize;
  uint32_t generation;
  Payload payload;
  uint32_t crc;
};
#pragma pack(pop)

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b)
      crc = (crc & 1U) ? (crc >> 1U) ^ 0xEDB88320UL : crc >> 1U;
  }
  return ~crc;
}

bool semanticValid(const Record& r) {
  return r.magic == MAGIC && r.schema == SCHEMA &&
         r.payloadSize == sizeof(Payload) && r.generation != 0 &&
         r.payload.volume <= 100 && r.payload.mqttPort != 0 &&
         r.payload.ecdhPolicy <= 1;
}

bool atomicRecordAccepted(const Record& r, uint8_t commit) {
  return commit == COMMIT &&
         crc32(reinterpret_cast<const uint8_t*>(&r), offsetof(Record, crc)) == r.crc &&
         semanticValid(r);
}
}

int main() {
  Record injected{};
  injected.magic = MAGIC;
  injected.schema = SCHEMA;
  injected.payloadSize = sizeof(Payload);
  injected.generation = 7;
  injected.payload.volume = 50;
  injected.payload.mqttPort = 8883;
  injected.payload.ecdhPolicy = 2; // CRC-valid but semantically invalid.
  injected.crc = crc32(reinterpret_cast<const uint8_t*>(&injected), offsetof(Record, crc));

  assert(crc32(reinterpret_cast<const uint8_t*>(&injected), offsetof(Record, crc)) == injected.crc);
  assert(!semanticValid(injected));
  assert(!atomicRecordAccepted(injected, COMMIT));

  injected.payload.ecdhPolicy = 1;
  injected.crc = crc32(reinterpret_cast<const uint8_t*>(&injected), offsetof(Record, crc));
  assert(atomicRecordAccepted(injected, COMMIT));
  return 0;
}
