#include "ReplayStore.h"
#include "BoardConfig.h"
#include <Preferences.h>
#include <Wire.h>
#include <cstring>

namespace {
constexpr uint32_t FRAM_MAGIC = 0x4652504CUL; // "FRPL"
constexpr uint32_t NVS_MAGIC = 0x524A5231UL;  // "RJR1"
constexpr char NVS_NAMESPACE[] = "fieldradio";
constexpr char NVS_HEAD_KEY[] = "rj_head";
}

uint32_t ReplayStore::crc32(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *p++;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
  }
  return ~crc;
}

bool ReplayStore::validEntry(const ReplayEntry& entry) const {
  return entry.sourceId != 0 &&
         entry.type <= Config::LORA_TYPE_NEIGHBOR_BEACON &&
         entry.bitmap != 0;
}

bool ReplayStore::begin() {
  backend_ = Backend::None;
  healthy_ = false;
  nvsGeneration_ = 0;
  nvsHead_ = 0;
  nvsValidRecords_ = 0;

  if (Config::REPLAY_STORE_BACKEND == Config::ReplayStoreBackend::BACKEND_FRAM) {
    if (beginFram()) return true;
    // A missing/unresponsive FRAM is a deployment condition, not a reason to
    // disable replay persistence. Fall back to the wear-levelled NVS journal.
  }
  return beginNvsJournal();
}

bool ReplayStore::beginFram() {
  Wire.begin(Board::FRAM_SDA, Board::FRAM_SCL, 400000);
  Wire.setTimeOut(50);
  Wire.beginTransmission(Config::REPLAY_FRAM_I2C_ADDR);
  if (Wire.endTransmission() != 0) return false;

  FramHeader header{};
  const bool headerRead = readFram(0, &header, sizeof(header));
  const bool blank = headerRead &&
      (header.magic == 0x00000000UL || header.magic == 0xFFFFFFFFUL);
  if (!headerRead) return false;
  if (blank) {
    header.magic = FRAM_MAGIC;
    header.version = Config::REPLAY_STORE_VERSION;
    header.entrySize = sizeof(ReplayEntry);
    header.slotSize = Config::REPLAY_FRAM_SLOT_BYTES;
    header.slotCount = Config::LORA_REPLAY_SOURCE_CACHE_SIZE;
    header.crc = crc32(&header, offsetof(FramHeader, crc));
    if (!writeFram(0, &header, sizeof(header))) return false;
  } else if (header.magic != FRAM_MAGIC ||
             header.version != Config::REPLAY_STORE_VERSION ||
             header.entrySize != sizeof(ReplayEntry) ||
             header.slotSize != Config::REPLAY_FRAM_SLOT_BYTES ||
             header.slotCount != Config::LORA_REPLAY_SOURCE_CACHE_SIZE ||
             header.crc != crc32(&header, offsetof(FramHeader, crc))) {
    // Never reinitialize a non-blank FRAM header on corruption: doing so
    // would silently erase the replay state and weaken anti-replay.
    return false;
  }

  backend_ = Backend::Fram;
  healthy_ = true;
  return true;
}

bool ReplayStore::beginNvsJournal() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  nvsHead_ = prefs.getUShort(NVS_HEAD_KEY, 0);
  nvsGeneration_ = prefs.getUInt("rj_gen", 0);
  prefs.end();
  if (nvsHead_ >= Config::REPLAY_NVS_JOURNAL_RECORDS) nvsHead_ = 0;
  backend_ = Backend::NvsJournal;
  healthy_ = true;
  return true;
}

bool ReplayStore::readFram(uint16_t address, void* data, size_t len) const {
  if (!data || !len || static_cast<uint32_t>(address) + len > Config::REPLAY_FRAM_SIZE_BYTES)
    return false;
  Wire.beginTransmission(Config::REPLAY_FRAM_I2C_ADDR);
  Wire.write(static_cast<uint8_t>(address >> 8));
  Wire.write(static_cast<uint8_t>(address));
  if (Wire.endTransmission(false) != 0) return false;
  const size_t requested = Wire.requestFrom(static_cast<int>(Config::REPLAY_FRAM_I2C_ADDR),
                                             static_cast<int>(len));
  if (requested != len) return false;
  uint8_t* p = static_cast<uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) p[i] = Wire.read();
  return true;
}

bool ReplayStore::writeFram(uint16_t address, const void* data, size_t len) const {
  if (!data || !len || static_cast<uint32_t>(address) + len > Config::REPLAY_FRAM_SIZE_BYTES)
    return false;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (len) {
    const size_t chunk = min<size_t>(Config::REPLAY_FRAM_WRITE_CHUNK_BYTES, len);
    Wire.beginTransmission(Config::REPLAY_FRAM_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(address >> 8));
    Wire.write(static_cast<uint8_t>(address));
    if (Wire.write(p, chunk) != chunk || Wire.endTransmission() != 0) return false;
    address = static_cast<uint16_t>(address + chunk);
    p += chunk;
    len -= chunk;
    if (Config::REPLAY_FRAM_WRITE_DELAY_MS) delay(Config::REPLAY_FRAM_WRITE_DELAY_MS);
  }
  return true;
}

bool ReplayStore::loadFram(ReplayEntry* out, size_t count) {
  if (!out || count != Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  memset(out, 0, sizeof(ReplayEntry) * count);
  for (size_t i = 0; i < count; ++i) {
    FramSlot slot{};
    const uint16_t address = static_cast<uint16_t>(Config::REPLAY_FRAM_HEADER_BYTES +
        i * Config::REPLAY_FRAM_SLOT_BYTES);
    if (!readFram(address, &slot, sizeof(slot))) return false;
    if (slot.crc != crc32(&slot.entry, sizeof(slot.entry)) || !validEntry(slot.entry)) continue;
    out[i] = slot.entry;
  }
  return true;
}

bool ReplayStore::loadNvsJournal(ReplayEntry* out, size_t count) {
  if (!out || count != Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  memset(out, 0, sizeof(ReplayEntry) * count);
  uint32_t newest[Config::LORA_REPLAY_SOURCE_CACHE_SIZE] = {};
  bool any = false;
  uint32_t maxGeneration = 0;
  uint16_t maxGenerationIndex = 0;
  uint16_t validCount = 0;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return false;
  for (size_t i = 0; i < Config::REPLAY_NVS_JOURNAL_RECORDS; ++i) {
    char key[8] = {};
    snprintf(key, sizeof(key), "rj%03u", static_cast<unsigned>(i));
    NvsRecord rec{};
    if (prefs.getBytes(key, &rec, sizeof(rec)) != sizeof(rec)) continue;
    if (rec.magic != NVS_MAGIC || rec.version != Config::REPLAY_STORE_VERSION ||
        rec.slot >= count || rec.crc != crc32(&rec, offsetof(NvsRecord, crc)) ||
        !validEntry(rec.entry)) continue;
    ++validCount;
    if (!any || static_cast<int32_t>(rec.generation - newest[rec.slot]) > 0) {
      out[rec.slot] = rec.entry;
      newest[rec.slot] = rec.generation;
      if (!any || static_cast<int32_t>(rec.generation - maxGeneration) > 0) {
        maxGeneration = rec.generation;
        maxGenerationIndex = static_cast<uint16_t>(i);
      }
      any = true;
    }
  }

  // Migrate the legacy fixed-slot NVS representation if the journal is empty.
  if (!any) {
    for (size_t i = 0; i < count; ++i) {
      char key[8] = {};
      snprintf(key, sizeof(key), "r%02u", static_cast<unsigned>(i));
      ReplayEntry legacy{};
      if (prefs.getBytes(key, &legacy, sizeof(legacy)) == sizeof(legacy) && validEntry(legacy))
        out[i] = legacy;
    }
  }
  prefs.end();
  nvsValidRecords_ = static_cast<uint8_t>(min<uint16_t>(validCount, 255));
  nvsGeneration_ = maxGeneration;
  if (any)
    nvsHead_ = static_cast<uint16_t>((maxGenerationIndex + 1U) % Config::REPLAY_NVS_JOURNAL_RECORDS);
  return true;
}

bool ReplayStore::load(ReplayEntry* out, size_t count) {
  if (!healthy_) return false;
  return backend_ == Backend::Fram ? loadFram(out, count) : loadNvsJournal(out, count);
}

bool ReplayStore::persistFram(const ReplayEntry& entry, size_t slot) {
  if (slot >= Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  FramSlot record{};
  record.entry = entry;
  record.crc = crc32(&record.entry, sizeof(record.entry));
  const uint16_t address = static_cast<uint16_t>(Config::REPLAY_FRAM_HEADER_BYTES +
      slot * Config::REPLAY_FRAM_SLOT_BYTES);
  return writeFram(address, &record, sizeof(record));
}

bool ReplayStore::compactNvsJournal(const ReplayEntry* entries, size_t count) {
  if (!entries || count != Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  for (size_t i = 0; i < Config::REPLAY_NVS_JOURNAL_RECORDS; ++i) {
    char key[8] = {};
    snprintf(key, sizeof(key), "rj%03u", static_cast<unsigned>(i));
    (void)prefs.remove(key);
  }
  nvsHead_ = 0;
  nvsValidRecords_ = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!validEntry(entries[i])) continue;
    NvsRecord rec{};
    rec.magic = NVS_MAGIC;
    rec.version = Config::REPLAY_STORE_VERSION;
    rec.slot = static_cast<uint8_t>(i);
    rec.generation = ++nvsGeneration_;
    rec.entry = entries[i];
    rec.crc = crc32(&rec, offsetof(NvsRecord, crc));
    char key[8] = {};
    snprintf(key, sizeof(key), "rj%03u", static_cast<unsigned>(nvsHead_));
    if (prefs.putBytes(key, &rec, sizeof(rec)) != sizeof(rec)) {
      prefs.end();
      return false;
    }
    nvsHead_ = static_cast<uint16_t>((nvsHead_ + 1U) % Config::REPLAY_NVS_JOURNAL_RECORDS);
    ++nvsValidRecords_;
  }
  (void)prefs.putUShort(NVS_HEAD_KEY, nvsHead_);
  (void)prefs.putUInt("rj_gen", nvsGeneration_);
  prefs.end();
  return true;
}

bool ReplayStore::persistNvsJournal(const ReplayEntry& entry, size_t slot) {
  if (slot >= Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  if (nvsValidRecords_ > (Config::REPLAY_NVS_JOURNAL_RECORDS * Config::REPLAY_NVS_COMPACT_PERCENT) / 100U) {
    prefs.end();
    return false; // caller performs a snapshot compaction; avoid recursive NVS access.
  }
  NvsRecord rec{};
  rec.magic = NVS_MAGIC;
  rec.version = Config::REPLAY_STORE_VERSION;
  rec.slot = static_cast<uint8_t>(slot);
  rec.generation = ++nvsGeneration_;
  rec.entry = entry;
  rec.crc = crc32(&rec, offsetof(NvsRecord, crc));
  char key[8] = {};
  snprintf(key, sizeof(key), "rj%03u", static_cast<unsigned>(nvsHead_));
  const bool ok = prefs.putBytes(key, &rec, sizeof(rec)) == sizeof(rec) &&
                  prefs.putUShort(NVS_HEAD_KEY, static_cast<uint16_t>((nvsHead_ + 1U) % Config::REPLAY_NVS_JOURNAL_RECORDS)) > 0 &&
                  prefs.putUInt("rj_gen", nvsGeneration_) > 0;
  prefs.end();
  if (ok) {
    nvsHead_ = static_cast<uint16_t>((nvsHead_ + 1U) % Config::REPLAY_NVS_JOURNAL_RECORDS);
    ++nvsValidRecords_;
  }
  return ok;
}

bool ReplayStore::persist(const ReplayEntry& entry, size_t slot) {
  if (!healthy_ || !validEntry(entry)) return false;
  if (backend_ == Backend::Fram) return persistFram(entry, slot);
  return persistNvsJournal(entry, slot);
}

bool ReplayStore::flushAll(const ReplayEntry* in, size_t count) {
  if (!healthy_ || !in || count != Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  if (backend_ == Backend::Fram) {
    for (size_t i = 0; i < count; ++i)
      if (validEntry(in[i]) && !persistFram(in[i], i)) return false;
    return true;
  }
  return compactNvsJournal(in, count);
}
