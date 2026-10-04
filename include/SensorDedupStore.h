#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "Config.h"

class SensorDedupStore {
public:
  enum class DedupResult : uint8_t {
    New,
    Duplicate,
    PersistenceFailure,
  };

  bool begin();
  bool healthy() const { return ready_; }
  // New commits the tuple; Duplicate means the tuple is already persisted;
  // PersistenceFailure means the MRAM journal could not be durably updated.
  DedupResult seenOrUpdate(uint32_t sourceId, uint16_t sensorId, uint32_t sourceSequence,
                           uint8_t schemaVersion, uint32_t firmwareVersion);
  DedupResult reserve(uint32_t sourceId, uint16_t sensorId, uint32_t sourceSequence,
                     uint8_t schemaVersion, uint32_t firmwareVersion);
  bool commit(uint32_t sourceId, uint16_t sensorId, uint32_t sourceSequence);
  bool abort(uint32_t sourceId, uint16_t sensorId, uint32_t sourceSequence);
  bool hasCommitted(uint32_t sourceId, uint16_t sensorId, uint32_t sourceSequence) const;

private:
  struct Entry {
    uint32_t sourceId = 0;
    uint16_t sensorId = 0;
    uint16_t reserved = 0;
    uint32_t sourceSequence = 0;
    uint8_t schemaVersion = 0;
    uint8_t reserved2[3] = {};
    uint32_t firmwareVersion = 0;
  } __attribute__((packed));

  struct Slot {
    Entry entry{};
    uint32_t generation = 0;
    uint32_t crc = 0;
    uint8_t reserved[8] = {};
  } __attribute__((packed));

  static_assert(sizeof(Slot) == Config::SENSOR_DEDUP_MRAM_SLOT_BYTES,
                "sensor dedup MRAM slot size mismatch");

  Entry entries_[Config::SENSOR_DEDUP_SLOT_COUNT]{};
  uint32_t generations_[Config::SENSOR_DEDUP_SLOT_COUNT]{};
  uint8_t valid_[Config::SENSOR_DEDUP_SLOT_COUNT]{};
  uint8_t pending_[Config::SENSOR_DEDUP_SLOT_COUNT]{};
  bool ready_ = false;

  static uint32_t crc32(const void* data, size_t len);
  bool readSlot(uint16_t address, Slot& slot) const;
  bool writeSlot(uint16_t address, const Slot& slot) const;
};
