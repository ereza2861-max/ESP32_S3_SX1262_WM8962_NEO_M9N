#pragma once

#include <Arduino.h>
#include <stddef.h>
#include "Config.h"

struct ReplayEntry {
  uint32_t sourceId = 0;
  uint16_t highestSeq = 0;
  uint8_t type = 0;
  uint32_t bitmap = 0;
  uint32_t highestPayloadHash = 0;
  uint32_t seenMs = 0;
  uint32_t lastEpochSec = 0;
};

class ReplayStore {
public:
  bool begin();
  bool load(ReplayEntry* out, size_t count);
  bool persist(const ReplayEntry& entry, size_t slot);
  bool flushAll(const ReplayEntry* in, size_t count);
  bool healthy() const { return healthy_; }

private:
  enum class Backend : uint8_t {
    None = 0,
    Mram,
    NvsJournal,
  };

  struct MramHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t entrySize;
    uint16_t slotSize;
    uint16_t slotCount;
    uint32_t crc;
  } __attribute__((packed));

  struct MramSlot {
    ReplayEntry entry;
    uint32_t crc;
    uint32_t generation;
  } __attribute__((packed));

  struct NvsRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t slot;
    uint16_t reserved;
    uint32_t generation;
    ReplayEntry entry;
    uint32_t crc;
  } __attribute__((packed));

  struct NvsSnapshot {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    uint32_t generation;
    uint16_t count;
    uint16_t reserved2;
    ReplayEntry entries[Config::LORA_REPLAY_SOURCE_CACHE_SIZE];
    uint32_t crc;
  } __attribute__((packed));

  static constexpr uint32_t NVS_SNAPSHOT_MAGIC = 0x52534E50UL; // "RSNP"
  static constexpr uint8_t NVS_SNAPSHOT_VERSION = 1;

  static_assert(sizeof(ReplayEntry) == 24, "ReplayEntry layout changed");
  static_assert(sizeof(MramSlot) == Config::REPLAY_MRAM_SLOT_BYTES,
                "MRAM replay slot size mismatch");

  Backend backend_ = Backend::None;
  bool healthy_ = false;
  uint32_t nvsGeneration_ = 0;
  uint16_t nvsHead_ = 0;
  uint8_t nvsValidRecords_ = 0;

  bool beginMram();
  bool beginNvsJournal();
  bool loadMram(ReplayEntry* out, size_t count);
  bool loadNvsJournal(ReplayEntry* out, size_t count);
  bool persistMram(const ReplayEntry& entry, size_t slot);
  bool persistNvsJournal(const ReplayEntry& entry, size_t slot);
  bool compactNvsJournal(const ReplayEntry* entries, size_t count);
  bool loadNvsSnapshot(ReplayEntry* out, size_t count, uint32_t& generation);
  bool readMram(uint16_t address, void* data, size_t len) const;
  bool writeMram(uint16_t address, const void* data, size_t len) const;
  bool validEntry(const ReplayEntry& entry) const;
  static uint32_t crc32(const void* data, size_t len);
};
