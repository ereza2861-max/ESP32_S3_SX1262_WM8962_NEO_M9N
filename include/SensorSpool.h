#pragma once

#include "SensorReader.h"
#include <cstddef>
#include <cstdint>
#include <type_traits>

class SensorSpool {
public:
  static constexpr uint8_t DELIVERY_MQTT = 1U << 0;
  static constexpr uint8_t DELIVERY_LORA = 1U << 1;
  static constexpr size_t MAX_RECORDS = 4096;
  static constexpr size_t MAX_BYTES = 1024UL * 1024UL;

  struct Pending {
    SensorReader::SensorSample sample{};
    uint32_t sampleId = 0;
    uint8_t requiredMask = 0;
    uint8_t deliveredMask = 0;
  };

  bool begin();
  bool append(const SensorReader::SensorSample& sample, uint8_t requiredMask);
  bool peek(Pending& out) const;
  bool markDelivered(uint32_t sampleId, uint8_t delivery);
  bool clear();
  bool flush();
  size_t depth() const { return count_; }
  uint32_t evictions() const { return evictions_; }
  uint32_t drops() const { return drops_; }
  uint32_t recovered() const { return recovered_; }
  bool ready() const { return ready_; }
  static uint32_t sampleId(const SensorReader::SensorSample& sample);
  String statusJson() const;

private:
  static constexpr char PATH[] = "/SENSOR/SPOOL.Q";
  static constexpr char TMP_PATH[] = "/SENSOR/SPOOL.TMP";
  static constexpr uint32_t MAGIC = 0x53504C51UL; // "SPLQ"
  static constexpr uint8_t VERSION = 1;
  static constexpr uint8_t TYPE_DATA = 1;
  static constexpr uint8_t TYPE_ACK = 2;

#pragma pack(push, 1)
  struct DiskRecord {
    uint32_t magic = MAGIC;
    uint8_t version = VERSION;
    uint8_t type = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    uint32_t recordId = 0;
    uint32_t sampleId = 0;
    uint16_t payloadLen = 0;
    uint16_t reserved2 = 0;
    SensorReader::SensorSample sample{};
    uint32_t crc32 = 0;
  };
#pragma pack(pop)
  static_assert(std::is_trivially_copyable<SensorReader::SensorSample>::value, "sensor sample must be wire-copyable");
  static_assert(sizeof(DiskRecord) <= 128, "sensor spool record unexpectedly large");

  struct IndexEntry {
    uint32_t recordId = 0;
    uint32_t sampleId = 0;
    uint32_t offset = 0;
    uint8_t requiredMask = 0;
    uint8_t deliveredMask = 0;
    bool valid = false;
  };

  bool scan();
  bool ensureDirectory() const;
  bool appendRecord(const DiskRecord& record);
  bool compact();
  bool evictOldest();
  static uint32_t crc32(const uint8_t* data, size_t len);
  static uint8_t requiredMaskFromFlags(uint8_t flags) { return flags & 0x03U; }
  static uint8_t deliveredMaskFromFlags(uint8_t flags) {
    return static_cast<uint8_t>((flags >> 2U) & 0x03U);
  }
  static uint8_t flagsFor(uint8_t requiredMask, uint8_t deliveredMask) {
    return static_cast<uint8_t>((requiredMask & 0x03U) |
                                ((deliveredMask & 0x03U) << 2U));
  }

  IndexEntry entries_[MAX_RECORDS]{};
  size_t count_ = 0;
  uint32_t nextRecordId_ = 1;
  uint32_t evictions_ = 0;
  uint32_t drops_ = 0;
  uint32_t recovered_ = 0;
  bool ready_ = false;
};
