#pragma once
#include <cstddef>
#include <cstdint>
#include "Config.h"

class SensorAckStore {
public:
  struct Record {
    uint32_t nodeId = 0;
    uint16_t sensorId = 0;
    uint32_t baseSequence = 0;
    uint32_t bitmap = 0;
  };
  bool begin();
  bool persist(const Record* records, size_t count);
  bool load(Record* out, size_t capacity, size_t& count) const;
  bool clear();

private:
  struct Header {
    uint32_t magic = 0x5341434BUL; // SACK
    uint16_t version = 1;
    uint16_t count = 0;
    uint32_t generation = 0;
    uint32_t crc = 0;
  } __attribute__((packed));
  static constexpr uint16_t BANK0 = 0x3040;
  static constexpr uint16_t BANK1 = 0x3240;
  static constexpr uint16_t BANK_BYTES = 512;
  static constexpr uint8_t BANK_COUNT = 2;
  static_assert(BANK1 + BANK_BYTES <= Config::MRAM_SIZE_BYTES, "sensor ACK MRAM out of range");
  static_assert(BANK0 + BANK_BYTES <= BANK1, "sensor ACK MRAM banks overlap");
  bool ready_ = false;
  uint32_t generation_ = 0;
  uint8_t activeBank_ = 0;
  static uint32_t crc32(const void* data, size_t len);
  bool readBank(uint8_t bank, Header& header,
                Record* out, size_t capacity,
                size_t& count) const;
  bool writeBank(uint8_t bank, uint32_t generation,
                 const Record* records, size_t count) const;
};
