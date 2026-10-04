#include "SensorDedupStore.h"
#include "MramStorage.h"

#include <cstring>

namespace {
uint16_t bankAddress(uint8_t bank, size_t slot) {
  const uint16_t base = bank == 0 ? Config::SENSOR_DEDUP_MRAM_BANK0_ADDR
                                  : Config::SENSOR_DEDUP_MRAM_BANK1_ADDR;
  return static_cast<uint16_t>(base + slot * Config::SENSOR_DEDUP_MRAM_SLOT_BYTES);
}
}

uint32_t SensorDedupStore::crc32(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *p++;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
  }
  return ~crc;
}

bool SensorDedupStore::readSlot(uint16_t address, Slot& slot) const {
  MramStorage& mram = MramStorage::shared();
  if (!mram.ready() && !mram.begin()) return false;
  if (!mram.read(address, &slot, sizeof(slot))) return false;
  return slot.entry.sourceId != 0 && slot.entry.sensorId != 0 &&
         slot.entry.sourceSequence != 0 && slot.generation != 0 &&
         slot.crc == crc32(&slot, offsetof(Slot, crc));
}

bool SensorDedupStore::writeSlot(uint16_t address, const Slot& slot) const {
  MramStorage& mram = MramStorage::shared();
  if (!mram.ready() && !mram.begin()) return false;
  return mram.write(address, &slot, sizeof(slot));
}

bool SensorDedupStore::begin() {
  ready_ = false;
  memset(entries_, 0, sizeof(entries_));
  memset(generations_, 0, sizeof(generations_));
  memset(valid_, 0, sizeof(valid_));
  memset(pending_, 0, sizeof(pending_));

  MramStorage& mram = MramStorage::shared();
  if (!mram.begin()) return false;

  for (size_t i = 0; i < Config::SENSOR_DEDUP_SLOT_COUNT; ++i) {
    Slot best{};
    bool have = false;
    for (uint8_t bank = 0; bank < Config::SENSOR_DEDUP_BANK_COUNT; ++bank) {
      Slot candidate{};
      if (!readSlot(bankAddress(bank, i), candidate)) continue;
      if ((candidate.entry.reserved & 0x0001U) != 0) continue;
      if (!have || static_cast<int32_t>(candidate.generation - best.generation) > 0) {
        best = candidate;
        have = true;
      }
    }
    if (have) {
      entries_[i] = best.entry;
      generations_[i] = best.generation;
      valid_[i] = 1;
    }
  }
  ready_ = true;
  return true;
}

bool SensorDedupStore::seenOrUpdate(uint32_t sourceId, uint16_t sensorId,
                                    uint32_t sourceSequence,
                                    uint8_t schemaVersion,
                                    uint32_t firmwareVersion) {
  if (!ready_ || sourceId == 0 || sensorId == 0 || sourceSequence == 0)
    return DedupResult::PersistenceFailure;
  size_t target = Config::SENSOR_DEDUP_SLOT_COUNT;
  uint32_t oldestGeneration = UINT32_MAX;
  for (size_t i = 0; i < Config::SENSOR_DEDUP_SLOT_COUNT; ++i) {
    if (valid_[i] && pending_[i] && entries_[i].sourceId == sourceId &&
        entries_[i].sensorId == sensorId &&
        entries_[i].sourceSequence == sourceSequence)
      return DedupResult::Duplicate;
    if (!valid_[i] || pending_[i]) {
      if (target == Config::SENSOR_DEDUP_SLOT_COUNT) target = i;
      continue;
    }
    if (entries_[i].sourceId == sourceId && entries_[i].sensorId == sensorId) {
      if (entries_[i].sourceSequence == sourceSequence) return DedupResult::Duplicate;
      if (static_cast<int32_t>(sourceSequence - entries_[i].sourceSequence) <= 0)
        return DedupResult::Duplicate;
      target = i;
      break;
    }
    if (generations_[i] < oldestGeneration) {
      oldestGeneration = generations_[i];
      if (target == Config::SENSOR_DEDUP_SLOT_COUNT) target = i;
    }
  }
  if (target == Config::SENSOR_DEDUP_SLOT_COUNT)
    return DedupResult::PersistenceFailure;

  Slot record{};
  record.entry.sourceId = sourceId;
  record.entry.sensorId = sensorId;
  record.entry.sourceSequence = sourceSequence;
  record.entry.schemaVersion = schemaVersion;
  record.entry.firmwareVersion = firmwareVersion;
  record.entry.reserved = 0x0001U;
  record.generation = generations_[target] + 1U;
  if (record.generation == 0) record.generation = 1;
  record.crc = crc32(&record, offsetof(Slot, crc));

  uint8_t currentBank = 0;
  Slot current{};
  bool haveCurrent = false;
  for (uint8_t bank = 0; bank < Config::SENSOR_DEDUP_BANK_COUNT; ++bank) {
    Slot candidate{};
    if (!readSlot(bankAddress(bank, target), candidate)) continue;
    if (!haveCurrent || static_cast<int32_t>(candidate.generation - current.generation) > 0) {
      current = candidate;
      currentBank = bank;
      haveCurrent = true;
    }
  }
  const uint8_t targetBank = haveCurrent ? static_cast<uint8_t>(currentBank ^ 1U) : 0U;
  if (!writeSlot(bankAddress(targetBank, target), record))
    return DedupResult::PersistenceFailure;

  entries_[target] = record.entry;
  generations_[target] = record.generation;
  valid_[target] = 1;
  pending_[target] = 1;
  return DedupResult::New;
}

SensorDedupStore::DedupResult SensorDedupStore::seenOrUpdate(
    uint32_t sourceId, uint16_t sensorId, uint32_t sourceSequence,
    uint8_t schemaVersion, uint32_t firmwareVersion) {
  const DedupResult result = reserve(sourceId, sensorId, sourceSequence,
                                     schemaVersion, firmwareVersion);
  if (result != DedupResult::New) return result;
  return commit(sourceId, sensorId, sourceSequence) ? DedupResult::New
                                                     : DedupResult::PersistenceFailure;
}

bool SensorDedupStore::commit(uint32_t sourceId, uint16_t sensorId,
                              uint32_t sourceSequence) {
  if (!ready_) return false;
  for (size_t i = 0; i < Config::SENSOR_DEDUP_SLOT_COUNT; ++i) {
    if (!valid_[i] || !pending_[i] || entries_[i].sourceId != sourceId ||
        entries_[i].sensorId != sensorId || entries_[i].sourceSequence != sourceSequence)
      continue;
    Slot record{};
    record.entry = entries_[i];
    record.entry.reserved &= static_cast<uint16_t>(~0x0001U);
    record.generation = generations_[i] + 1U;
    if (record.generation == 0) record.generation = 1;
    record.crc = crc32(&record, offsetof(Slot, crc));
    uint8_t currentBank = 0;
    Slot current{};
    bool haveCurrent = false;
    for (uint8_t bank = 0; bank < Config::SENSOR_DEDUP_BANK_COUNT; ++bank) {
      Slot candidate{};
      if (!readSlot(bankAddress(bank, i), candidate)) continue;
      if (!haveCurrent || static_cast<int32_t>(candidate.generation - current.generation) > 0) {
        current = candidate; currentBank = bank; haveCurrent = true;
      }
    }
    const uint8_t targetBank = haveCurrent ? static_cast<uint8_t>(currentBank ^ 1U) : 0U;
    if (!writeSlot(bankAddress(targetBank, i), record)) return false;
    entries_[i] = record.entry;
    generations_[i] = record.generation;
    pending_[i] = 0;
    return true;
  }
  return false;
}

bool SensorDedupStore::abort(uint32_t sourceId, uint16_t sensorId,
                             uint32_t sourceSequence) {
  if (!ready_) return false;
  for (size_t i = 0; i < Config::SENSOR_DEDUP_SLOT_COUNT; ++i) {
    if (!valid_[i] || !pending_[i] || entries_[i].sourceId != sourceId ||
        entries_[i].sensorId != sensorId || entries_[i].sourceSequence != sourceSequence)
      continue;
    entries_[i] = {};
    generations_[i] = 0;
    valid_[i] = 0;
    pending_[i] = 0;
    // A pending record is never visible after reboot because begin() ignores it.
    return true;
  }
  return false;
}

bool SensorDedupStore::hasCommitted(uint32_t sourceId, uint16_t sensorId,
                                    uint32_t sourceSequence) const {
  for (size_t i = 0; i < Config::SENSOR_DEDUP_SLOT_COUNT; ++i)
    if (valid_[i] && !pending_[i] && entries_[i].sourceId == sourceId &&
        entries_[i].sensorId == sensorId && entries_[i].sourceSequence == sourceSequence)
      return true;
  return false;
}
