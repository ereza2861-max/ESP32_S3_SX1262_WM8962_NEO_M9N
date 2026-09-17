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
constexpr char NVS_SNAPSHOT_NS_A[] = "fr_rj_a";
constexpr char NVS_SNAPSHOT_NS_B[] = "fr_rj_b";
constexpr char NVS_SNAPSHOT_KEY[] = "snapshot";
static_assert(Config::REPLAY_FRAM_HEADER_BYTES +
                  Config::LORA_REPLAY_SOURCE_CACHE_SIZE * 2U *
                  Config::REPLAY_FRAM_SLOT_BYTES <= Config::REPLAY_FRAM_SIZE_BYTES,
              "Rev-C FRAM replay double-buffer does not fit");
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
  if (!gI2cMutex) return false;
  Wire.begin(Board::FRAM_SDA, Board::FRAM_SCL, 400000);
  Wire.setTimeOut(Config::I2C_TIMEOUT_MS);
  {
    I2cLock lock(pdMS_TO_TICKS(Config::I2C_TIMEOUT_MS));
    if (!lock.ok()) return false;
    Wire.beginTransmission(Config::REPLAY_FRAM_I2C_ADDR);
    if (Wire.endTransmission() != 0) return false;
  }

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
  I2cLock lock(pdMS_TO_TICKS(Config::I2C_TIMEOUT_MS));
  if (!lock.ok()) return false;
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
  I2cLock lock(pdMS_TO_TICKS(Config::I2C_TIMEOUT_MS));
  if (!lock.ok()) return false;
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
    bool haveValid = false;
    uint32_t newestGeneration = 0;
    ReplayEntry newestEntry{};

    // Rev-C uses two physical banks per logical replay slot. Bank 0 preserves
    // the original fixed-slot addresses so existing FRAM state remains readable;
    // bank 1 is the new shadow bank. A torn write to the inactive bank therefore
    // cannot destroy the last committed anti-replay state. generation==0 records
    // retain the legacy CRC format for migration.
    for (uint8_t bank = 0; bank < 2; ++bank) {
      FramSlot slot{};
      const uint32_t physicalSlot =
          bank == 0 ? static_cast<uint32_t>(i)
                    : static_cast<uint32_t>(Config::LORA_REPLAY_SOURCE_CACHE_SIZE) + i;
      const uint16_t address = static_cast<uint16_t>(
          Config::REPLAY_FRAM_HEADER_BYTES +
          physicalSlot * Config::REPLAY_FRAM_SLOT_BYTES);
      if (!readFram(address, &slot, sizeof(slot))) return false;

      bool valid = false;
      if (slot.generation == 0) {
        valid = slot.crc == crc32(&slot.entry, sizeof(slot.entry));
      } else {
        uint8_t commitData[sizeof(ReplayEntry) + sizeof(uint32_t)] = {};
        memcpy(commitData, &slot.entry, sizeof(slot.entry));
        memcpy(commitData + sizeof(slot.entry), &slot.generation, sizeof(slot.generation));
        valid = slot.crc == crc32(commitData, sizeof(commitData));
      }
      if (!valid || !validEntry(slot.entry)) continue;

      if (!haveValid ||
          static_cast<int32_t>(slot.generation - newestGeneration) > 0) {
        haveValid = true;
        newestGeneration = slot.generation;
        newestEntry = slot.entry;
      }
    }
    if (haveValid) out[i] = newestEntry;
  }
  return true;
}

bool ReplayStore::loadNvsSnapshot(ReplayEntry* out, size_t count, uint32_t& generation) {
  if (!out || count != Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  generation = 0;
  bool have = false;
  const char* namespaces[] = {NVS_SNAPSHOT_NS_A, NVS_SNAPSHOT_NS_B};
  for (const char* ns : namespaces) {
    Preferences prefs;
    if (!prefs.begin(ns, true)) continue;
    NvsSnapshot snap{};
    const size_t got = prefs.getBytes(NVS_SNAPSHOT_KEY, &snap, sizeof(snap));
    prefs.end();
    if (got != sizeof(snap) ||
        snap.magic != NVS_SNAPSHOT_MAGIC ||
        snap.version != NVS_SNAPSHOT_VERSION ||
        snap.count != count ||
        snap.crc != crc32(&snap, offsetof(NvsSnapshot, crc)))
      continue;
    if (!have || static_cast<int32_t>(snap.generation - generation) > 0) {
      memcpy(out, snap.entries, sizeof(snap.entries));
      generation = snap.generation;
      have = true;
    }
  }
  return have;
}

bool ReplayStore::loadNvsJournal(ReplayEntry* out, size_t count) {
  if (!out || count != Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;
  memset(out, 0, sizeof(ReplayEntry) * count);

  uint32_t snapshotGeneration = 0;
  const bool haveSnapshot = loadNvsSnapshot(out, count, snapshotGeneration);
  uint32_t newest[Config::LORA_REPLAY_SOURCE_CACHE_SIZE] = {};
  uint32_t maxGeneration = snapshotGeneration;
  uint16_t maxGenerationIndex = 0;
  uint16_t validCount = 0;
  bool any = haveSnapshot;

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return false;
  for (size_t i = 0; i < Config::REPLAY_NVS_JOURNAL_RECORDS; ++i) {
    char key[8] = {};
    snprintf(key, sizeof(key), "rj%03u", static_cast<unsigned>(i));
    NvsRecord rec{};
    if (prefs.getBytes(key, &rec, sizeof(rec)) != sizeof(rec)) continue;
    if (rec.magic != NVS_MAGIC || rec.version != Config::REPLAY_STORE_VERSION ||
        rec.slot >= count || rec.crc != crc32(&rec, offsetof(NvsRecord, crc)) ||
        !validEntry(rec.entry) || rec.generation <= snapshotGeneration) continue;

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
  const uint16_t savedHead = prefs.getUShort(NVS_HEAD_KEY, 0);
  const uint32_t savedGeneration = prefs.getUInt("rj_gen", 0);
  prefs.end();

  // If a committed snapshot exists, its generation is the recovery baseline.
  // A torn/old journal record must never overwrite it.
  nvsGeneration_ = max(maxGeneration, savedGeneration);
  nvsValidRecords_ = static_cast<uint8_t>(min<uint16_t>(validCount, 255));
  if (any && maxGenerationIndex < Config::REPLAY_NVS_JOURNAL_RECORDS)
    nvsHead_ = static_cast<uint16_t>((maxGenerationIndex + 1U) %
                                     Config::REPLAY_NVS_JOURNAL_RECORDS);
  else
    nvsHead_ = savedHead < Config::REPLAY_NVS_JOURNAL_RECORDS ? savedHead : 0;

  // Migrate the legacy fixed-slot NVS representation only when neither the
  // transactional snapshot nor the journal contains usable state.
  if (!any) {
    Preferences legacy;
    if (!legacy.begin(NVS_NAMESPACE, true)) return false;
    for (size_t i = 0; i < count; ++i) {
      char key[8] = {};
      snprintf(key, sizeof(key), "r%02u", static_cast<unsigned>(i));
      ReplayEntry entry{};
      if (legacy.getBytes(key, &entry, sizeof(entry)) == sizeof(entry) &&
          validEntry(entry))
        out[i] = entry;
    }
    legacy.end();
  }
  return true;
}

bool ReplayStore::load(ReplayEntry* out, size_t count) {
  if (!healthy_) return false;
  return backend_ == Backend::Fram ? loadFram(out, count) : loadNvsJournal(out, count);
}

bool ReplayStore::persistFram(const ReplayEntry& entry, size_t slot) {
  if (slot >= Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;

  uint32_t newestGeneration = 0;
  uint8_t newestBank = 0;
  bool haveValid = false;
  for (uint8_t bank = 0; bank < 2; ++bank) {
    FramSlot current{};
    const uint32_t physicalSlot =
        bank == 0 ? static_cast<uint32_t>(slot)
                  : static_cast<uint32_t>(Config::LORA_REPLAY_SOURCE_CACHE_SIZE) + slot;
    const uint16_t address = static_cast<uint16_t>(
        Config::REPLAY_FRAM_HEADER_BYTES +
        physicalSlot * Config::REPLAY_FRAM_SLOT_BYTES);
    if (!readFram(address, &current, sizeof(current))) return false;

    bool valid = false;
    if (current.generation == 0) {
      valid = current.crc == crc32(&current.entry, sizeof(current.entry));
    } else {
      uint8_t commitData[sizeof(ReplayEntry) + sizeof(uint32_t)] = {};
      memcpy(commitData, &current.entry, sizeof(current.entry));
      memcpy(commitData + sizeof(current.entry), &current.generation,
             sizeof(current.generation));
      valid = current.crc == crc32(commitData, sizeof(commitData));
    }
    if (valid && validEntry(current.entry) &&
        (!haveValid || static_cast<int32_t>(current.generation - newestGeneration) > 0)) {
      haveValid = true;
      newestGeneration = current.generation;
      newestBank = bank;
    }
  }

  uint32_t generation = newestGeneration + 1U;
  if (generation == 0) generation = 1;  // reserve 0 for legacy records
  const uint8_t targetBank = haveValid ? static_cast<uint8_t>(newestBank ^ 1U) : 0U;

  FramSlot record{};
  record.entry = entry;
  record.generation = generation;
  uint8_t commitData[sizeof(ReplayEntry) + sizeof(uint32_t)] = {};
  memcpy(commitData, &record.entry, sizeof(record.entry));
  memcpy(commitData + sizeof(record.entry), &record.generation,
         sizeof(record.generation));
  record.crc = crc32(commitData, sizeof(commitData));

  const uint32_t physicalSlot = static_cast<uint32_t>(slot) * 2U + targetBank;
  const uint16_t address = static_cast<uint16_t>(
      Config::REPLAY_FRAM_HEADER_BYTES +
      physicalSlot * Config::REPLAY_FRAM_SLOT_BYTES);
  return writeFram(address, &record, sizeof(record));
}

bool ReplayStore::compactNvsJournal(const ReplayEntry* entries, size_t count) {
  if (!entries || count != Config::LORA_REPLAY_SOURCE_CACHE_SIZE) return false;

  // Transactional two-bank snapshot:
  // 1) write the complete snapshot to the bank with the older generation;
  // 2) read it back and verify its CRC;
  // 3) only then delete the old journal records.
  // A power loss before step 3 leaves the previous journal intact; a power
  // loss after step 3 leaves the committed snapshot intact.
  auto snapshotGeneration = [&](const char* ns) -> uint32_t {
    Preferences prefs;
    if (!prefs.begin(ns, true)) return 0;
    NvsSnapshot snap{};
    const bool ok = prefs.getBytes(NVS_SNAPSHOT_KEY, &snap, sizeof(snap)) == sizeof(snap) &&
                    snap.magic == NVS_SNAPSHOT_MAGIC &&
                    snap.version == NVS_SNAPSHOT_VERSION &&
                    snap.count == count &&
                    snap.crc == crc32(&snap, offsetof(NvsSnapshot, crc));
    prefs.end();
    return ok ? snap.generation : 0;
  };
  const uint32_t genA = snapshotGeneration(NVS_SNAPSHOT_NS_A);
  const uint32_t genB = snapshotGeneration(NVS_SNAPSHOT_NS_B);
  const bool writeB = static_cast<int32_t>(genA - genB) <= 0;
  const char* targetNs = writeB ? NVS_SNAPSHOT_NS_B : NVS_SNAPSHOT_NS_A;
  const uint32_t generation = max(nvsGeneration_, max(genA, genB)) + 1U;

  NvsSnapshot snap{};
  snap.magic = NVS_SNAPSHOT_MAGIC;
  snap.version = NVS_SNAPSHOT_VERSION;
  snap.generation = generation;
  snap.count = static_cast<uint16_t>(count);
  memcpy(snap.entries, entries, sizeof(snap.entries));
  snap.crc = crc32(&snap, offsetof(NvsSnapshot, crc));

  {
    Preferences prefs;
    if (!prefs.begin(targetNs, false)) return false;
    if (prefs.putBytes(NVS_SNAPSHOT_KEY, &snap, sizeof(snap)) != sizeof(snap)) {
      prefs.end();
      return false;
    }
    prefs.end();
  }

  // Verify the committed bank before touching the old journal.
  NvsSnapshot verify{};
  {
    Preferences prefs;
    if (!prefs.begin(targetNs, true)) return false;
    const bool ok = prefs.getBytes(NVS_SNAPSHOT_KEY, &verify, sizeof(verify)) == sizeof(verify);
    prefs.end();
    if (!ok || verify.magic != NVS_SNAPSHOT_MAGIC ||
        verify.version != NVS_SNAPSHOT_VERSION ||
        verify.generation != generation ||
        verify.count != count ||
        verify.crc != crc32(&verify, offsetof(NvsSnapshot, crc)))
      return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  for (size_t i = 0; i < Config::REPLAY_NVS_JOURNAL_RECORDS; ++i) {
    char key[8] = {};
    snprintf(key, sizeof(key), "rj%03u", static_cast<unsigned>(i));
    (void)prefs.remove(key);
  }
  nvsGeneration_ = generation;
  nvsHead_ = 0;
  nvsValidRecords_ = 0;
  const bool metaOk = prefs.putUShort(NVS_HEAD_KEY, nvsHead_) > 0 &&
                      prefs.putUInt("rj_gen", nvsGeneration_) > 0;
  prefs.end();
  return metaOk;
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
