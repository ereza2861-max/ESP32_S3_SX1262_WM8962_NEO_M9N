#pragma once
#include <stddef.h>
#include <stdint.h>

namespace SensorBatchAck {
constexpr uint8_t MAGIC = 0xBA;
constexpr uint8_t VERSION = 1;
constexpr size_t MAX_RECORDS = 12;
constexpr size_t RECORD_BYTES = 14;

struct Record {
  uint32_t nodeId = 0;
  uint16_t sensorId = 0;
  uint32_t baseSequence = 0;
  uint32_t bitmap = 0;
};

inline size_t encode(const Record* records, size_t count, uint8_t* out, size_t capacity) {
  if (!records || !out || count == 0 || count > MAX_RECORDS ||
      capacity < 2U + 1U + count * RECORD_BYTES) return 0;
  out[0] = MAGIC;
  out[1] = VERSION;
  out[2] = static_cast<uint8_t>(count);
  for (size_t i = 0; i < count; ++i) {
    const size_t off = 3U + i * RECORD_BYTES;
    for (uint8_t b = 0; b < 4; ++b) out[off + b] = static_cast<uint8_t>(records[i].nodeId >> (8U * b));
    out[off + 4] = static_cast<uint8_t>(records[i].sensorId);
    out[off + 5] = static_cast<uint8_t>(records[i].sensorId >> 8);
    for (uint8_t b = 0; b < 4; ++b) out[off + 6 + b] = static_cast<uint8_t>(records[i].baseSequence >> (8U * b));
    for (uint8_t b = 0; b < 4; ++b) out[off + 10 + b] = static_cast<uint8_t>(records[i].bitmap >> (8U * b));
  }
  return 3U + count * RECORD_BYTES;
}

inline bool decode(const uint8_t* in, size_t len, Record* records, size_t capacity, size_t& count) {
  count = 0;
  if (!in || !records || len < 3 || in[0] != MAGIC || in[1] != VERSION ||
      in[2] == 0 || in[2] > MAX_RECORDS || capacity < in[2] ||
      len != 3U + static_cast<size_t>(in[2]) * RECORD_BYTES) return false;
  count = in[2];
  for (size_t i = 0; i < count; ++i) {
    const size_t off = 3U + i * RECORD_BYTES;
    records[i].nodeId = static_cast<uint32_t>(in[off]) |
                        (static_cast<uint32_t>(in[off + 1]) << 8) |
                        (static_cast<uint32_t>(in[off + 2]) << 16) |
                        (static_cast<uint32_t>(in[off + 3]) << 24);
    records[i].sensorId = static_cast<uint16_t>(in[off + 4]) |
                          (static_cast<uint16_t>(in[off + 5]) << 8);
    records[i].baseSequence = static_cast<uint32_t>(in[off + 6]) |
                              (static_cast<uint32_t>(in[off + 7]) << 8) |
                              (static_cast<uint32_t>(in[off + 8]) << 16) |
                              (static_cast<uint32_t>(in[off + 9]) << 24);
    records[i].bitmap = static_cast<uint32_t>(in[off + 10]) |
                        (static_cast<uint32_t>(in[off + 11]) << 8) |
                        (static_cast<uint32_t>(in[off + 12]) << 16) |
                        (static_cast<uint32_t>(in[off + 13]) << 24);
    if (records[i].nodeId == 0 || records[i].sensorId == 0 ||
        records[i].baseSequence == 0 || records[i].bitmap == 0) {
      count = 0;
      return false;
    }
  }
  return true;
}
}
