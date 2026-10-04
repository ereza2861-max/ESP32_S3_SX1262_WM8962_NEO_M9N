#include "SensorAckStore.h"
#include "MramStorage.h"
#include <cstring>

uint32_t SensorAckStore::crc32(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *p++;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
  }
  return ~crc;
}

bool SensorAckStore::readBank(uint8_t bank, Header& header,
                              SensorAckStore::Record* out,
                              size_t capacity, size_t& count) const {
  count = 0;
  MramStorage& mram = MramStorage::shared();
  if (!mram.ready() && !mram.begin()) return false;
  if (!mram.read(bank ? BANK1 : BANK0, &header, sizeof(header))) return false;
  if (header.magic != 0x5341434BUL || header.version != 1 ||
      header.count > Config::SENSOR_BATCH_ACK_MAX_RECORDS ||
      sizeof(Header) + header.count * sizeof(SensorAckStore::Record) > BANK_BYTES)
    return false;
  const size_t bytes = header.count * sizeof(SensorAckStore::Record);
  SensorAckStore::Record records[Config::SENSOR_BATCH_ACK_MAX_RECORDS]{};
  if (bytes && !mram.read(static_cast<uint16_t>((bank ? BANK1 : BANK0) + sizeof(Header)),
                           records, bytes)) return false;
  const uint32_t stored = header.crc;
  header.crc = 0;
  const uint32_t calculated = crc32(&header, sizeof(header));
  const uint32_t recordsCrc = crc32(records, bytes);
  header.crc = stored;
  if (stored != (calculated ^ recordsCrc)) return false;
  if (out && capacity) {
    count = header.count < capacity ? header.count : capacity;
    if (count) std::memcpy(out, records, count * sizeof(records[0]));
  } else {
    count = header.count;
  }
  return true;
}

bool SensorAckStore::writeBank(uint8_t bank, uint32_t generation,
                               const SensorAckStore::Record* records,
                               size_t count) const {
  if (count > Config::SENSOR_BATCH_ACK_MAX_RECORDS) return false;
  Header header{};
  header.count = static_cast<uint16_t>(count);
  header.generation = generation ? generation : 1;
  const size_t bytes = count * sizeof(SensorAckStore::Record);
  header.crc = 0;
  header.crc = crc32(&header, sizeof(header)) ^
              crc32(records, bytes);
  MramStorage& mram = MramStorage::shared();
  if (!mram.ready() && !mram.begin()) return false;
  const uint16_t base = bank ? BANK1 : BANK0;
  if (bytes && !mram.write(static_cast<uint16_t>(base + sizeof(Header)), records, bytes)) return false;
  return mram.write(base, &header, sizeof(header));
}

bool SensorAckStore::begin() {
  ready_ = false;
  Header best{};
  bool have = false;
  uint8_t bestBank = 0;
  for (uint8_t bank = 0; bank < BANK_COUNT; ++bank) {
    Header candidate{};
    size_t ignored = 0;
    if (!readBank(bank, candidate, nullptr, 0, ignored)) continue;
    if (!have || static_cast<int32_t>(candidate.generation - best.generation) > 0) {
      best = candidate; bestBank = bank; have = true;
    }
  }
  if (have) {
    generation_ = best.generation;
    activeBank_ = bestBank;
  }
  ready_ = MramStorage::shared().ready();
  return ready_;
}

bool SensorAckStore::persist(const SensorAckStore::Record* records, size_t count) {
  if (!ready_ || !records || count > Config::SENSOR_BATCH_ACK_MAX_RECORDS) return false;
  const uint8_t target = static_cast<uint8_t>(activeBank_ ^ 1U);
  const uint32_t next = generation_ + 1U ? generation_ + 1U : 1U;
  if (!writeBank(target, next, records, count)) return false;
  activeBank_ = target;
  generation_ = next;
  return true;
}

bool SensorAckStore::load(SensorAckStore::Record* out, size_t capacity, size_t& count) const {
  count = 0;
  if (!ready_) return false;
  Header header{};
  return readBank(activeBank_, header, out, capacity, count);
}

bool SensorAckStore::clear() {
  if (!ready_) return false;
  return persist(nullptr, 0);
}
