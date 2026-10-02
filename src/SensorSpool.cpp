#include "SensorSpool.h"
#include "AppState.h"
#include "StorageManager.h"

extern StorageManager storage;
#include <SD.h>
#include <cstring>
#include <cmath>

namespace {
constexpr uint32_t FNV_OFFSET = 2166136261UL;
constexpr uint32_t FNV_PRIME = 16777619UL;

void hashBytes(uint32_t& hash, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
}
}

bool SensorSpool::readDiskRecord(File& file, size_t offset, DiskRecord& out, size_t& recordSize) {
  if (!file.seek(offset)) return false;
  const size_t prefixLen = offsetof(DiskRecord, sample);
  uint8_t prefix[offsetof(DiskRecord, sample)] = {};
  if (file.read(prefix, prefixLen) != prefixLen) return false;
  uint8_t version = prefix[offsetof(DiskRecord, version)];
  if (version != VERSION && version != LEGACY_VERSION_V2 && version != LEGACY_VERSION) return false;
  uint32_t magic = 0;
  std::memcpy(&magic, prefix, sizeof(magic));
  if (magic != MAGIC) return false;
  out = {};
  std::memcpy(reinterpret_cast<uint8_t*>(&out), prefix, prefixLen);
  if (version == VERSION) {
    if (file.read(reinterpret_cast<uint8_t*>(&out.sample), sizeof(out.sample)) != sizeof(out.sample) ||
        file.read(reinterpret_cast<uint8_t*>(&out.sourceSequence), sizeof(out.sourceSequence)) != sizeof(out.sourceSequence) ||
        file.read(reinterpret_cast<uint8_t*>(&out.schemaVersion), sizeof(out.schemaVersion)) != sizeof(out.schemaVersion) ||
        file.read(reinterpret_cast<uint8_t*>(&out.firmwareVersion), sizeof(out.firmwareVersion)) != sizeof(out.firmwareVersion) ||
        file.read(reinterpret_cast<uint8_t*>(&out.crc32), sizeof(out.crc32)) != sizeof(out.crc32))
      return false;
    recordSize = sizeof(DiskRecord);
  } else if (version == LEGACY_VERSION_V2) {
    LegacyDiskRecordV2 legacy{};
    std::memcpy(reinterpret_cast<uint8_t*>(&legacy), prefix, prefixLen);
    if (file.read(reinterpret_cast<uint8_t*>(&legacy.sample), sizeof(legacy.sample)) != sizeof(legacy.sample) ||
        file.read(reinterpret_cast<uint8_t*>(&legacy.sourceSequence), sizeof(legacy.sourceSequence)) != sizeof(legacy.sourceSequence) ||
        file.read(reinterpret_cast<uint8_t*>(&legacy.crc32), sizeof(legacy.crc32)) != sizeof(legacy.crc32))
      return false;
    out.sourceSequence = legacy.sourceSequence;
    out.schemaVersion = 0;
    out.firmwareVersion = 0;
    out.sample = legacy.sample;
    out.crc32 = legacy.crc32;
    recordSize = sizeof(LegacyDiskRecordV2);
  } else {
    LegacyDiskRecord legacy{};
    std::memcpy(reinterpret_cast<uint8_t*>(&legacy), prefix, prefixLen);
    if (file.read(reinterpret_cast<uint8_t*>(&legacy.sample), sizeof(legacy.sample)) != sizeof(legacy.sample) ||
        file.read(reinterpret_cast<uint8_t*>(&legacy.crc32), sizeof(legacy.crc32)) != sizeof(legacy.crc32))
      return false;
    out.sourceSequence = 0;
    out.schemaVersion = 0;
    out.firmwareVersion = 0;
    out.sample = legacy.sample;
    out.crc32 = legacy.crc32;
    recordSize = sizeof(LegacyDiskRecord);
  }
  const uint32_t expected = crc32(
      reinterpret_cast<const uint8_t*>(&out),
      offsetof(DiskRecord, crc32));
  // Legacy CRCs cover their historical record layout, not the widened V3 record.
  if (version == LEGACY_VERSION) {
    LegacyDiskRecord legacy{};
    std::memcpy(reinterpret_cast<uint8_t*>(&legacy), reinterpret_cast<const uint8_t*>(&out),
                offsetof(LegacyDiskRecord, sample));
    legacy.sample = out.sample;
    legacy.crc32 = out.crc32;
    if (legacy.crc32 != crc32(reinterpret_cast<const uint8_t*>(&legacy),
                               offsetof(LegacyDiskRecord, crc32))) return false;
  } else if (version == LEGACY_VERSION_V2) {
    LegacyDiskRecordV2 legacy{};
    std::memcpy(reinterpret_cast<uint8_t*>(&legacy), reinterpret_cast<const uint8_t*>(&out),
                offsetof(LegacyDiskRecordV2, sample));
    legacy.sample = out.sample;
    legacy.sourceSequence = out.sourceSequence;
    legacy.crc32 = out.crc32;
    if (legacy.crc32 != crc32(reinterpret_cast<const uint8_t*>(&legacy),
                               offsetof(LegacyDiskRecordV2, crc32))) return false;
  } else if (out.crc32 != expected) {
    return false;
  }
  return true;
}

uint32_t SensorSpool::crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xEDB88320U & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U))));
  }
  return crc ^ 0xFFFFFFFFU;
}

uint32_t SensorSpool::sampleId(const SensorReader::SensorSample& sample) {
  uint32_t hash = FNV_OFFSET;
  hashBytes(hash, reinterpret_cast<const uint8_t*>(&sample.nodeId), sizeof(sample.nodeId));
  hashBytes(hash, reinterpret_cast<const uint8_t*>(&sample.sensorId), sizeof(sample.sensorId));
  hashBytes(hash, reinterpret_cast<const uint8_t*>(&sample.timestampMs), sizeof(sample.timestampMs));
  hashBytes(hash, reinterpret_cast<const uint8_t*>(&sample.value), sizeof(sample.value));
  return hash ? hash : 1U;
}

bool SensorSpool::ensureDirectory() const {
  if (!SD.exists("/SENSOR")) return SD.mkdir("/SENSOR");
  return true;
}

bool SensorSpool::appendRecord(const DiskRecord& input) {
  DiskRecord record = input;
  record.crc32 = crc32(reinterpret_cast<const uint8_t*>(&record),
                       offsetof(DiskRecord, crc32));
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return false;
  File file = SD.open(PATH, FILE_APPEND);
  if (!file) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
  file.flush();
  file.close();
  return written == sizeof(record);
}

bool SensorSpool::scan() {
  count_ = 0;
  nextRecordId_ = 1;
  recovered_ = 0;
  if (!SD.exists(PATH)) return true;

  SpiLock lock(pdMS_TO_TICKS(200));
  if (!lock.ok()) return false;
  File file = SD.open(PATH, FILE_READ);
  if (!file) return false;

  size_t validBytes = 0;
  while (file.available()) {
    const size_t start = static_cast<size_t>(file.position());
    DiskRecord record{};
    size_t recordSize = 0;
    if (!readDiskRecord(file, start, record, recordSize)) break;
    if (record.payloadLen != (record.type == TYPE_DATA ? sizeof(SensorReader::SensorSample) : 0) ||
        (record.type != TYPE_DATA && record.type != TYPE_ACK)) {
      break;
    }
    validBytes = start + recordSize;
    if (record.recordId >= nextRecordId_) nextRecordId_ = record.recordId + 1U;
    if (record.type == TYPE_DATA) {
      if (record.sampleId == 0 || requiredMaskFromFlags(record.flags) == 0) continue;
      if (count_ >= MAX_RECORDS) {
        size_t selected = 0;
        for (size_t i = 1; i < count_; ++i) {
          if (entries_[i].priority < entries_[selected].priority ||
              (entries_[i].priority == entries_[selected].priority &&
               entries_[i].recordId < entries_[selected].recordId)) selected = i;
        }
        for (size_t i = selected + 1; i < count_; ++i) entries_[i - 1] = entries_[i];
        entries_[count_ - 1] = {};
        --count_;
        ++evictions_;
      }
      IndexEntry& entry = entries_[count_++];
      entry = {};
      entry.recordId = record.recordId;
      entry.sampleId = record.sampleId;
      entry.offset = static_cast<uint32_t>(start);
      entry.requiredMask = requiredMaskFromFlags(record.flags);
      entry.deliveredMask = deliveredMaskFromFlags(record.flags);
      entry.source = record.source <= REMOTE_LORA ? record.source : LOCAL_BLE;
      entry.priority = static_cast<uint8_t>(record.priority & 0xFFU);
      entry.sourceSequence = record.sourceSequence;
      entry.recordSize = recordSize;
      entry.valid = true;
      if (entry.deliveredMask == entry.requiredMask) --count_;
    } else if (record.type == TYPE_ACK) {
      for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].valid && entries_[i].sampleId == record.sampleId) {
          entries_[i].deliveredMask = deliveredMaskFromFlags(record.flags);
          if (entries_[i].deliveredMask == entries_[i].requiredMask) {
            for (size_t j = i + 1; j < count_; ++j) entries_[j - 1] = entries_[j];
            entries_[count_ - 1] = {};
            --count_;
          }
          break;
        }
      }
    } else {
      break;
    }
  }
  file.close();

  SpiLock truncateLock(pdMS_TO_TICKS(100));
  if (truncateLock.ok() && SD.exists(PATH)) {
    File repair = SD.open(PATH, FILE_WRITE);
    if (repair) {
      (void)repair.truncate(validBytes);
      repair.close();
    }
  }
  return true;
}
bool SensorSpool::compact() {
  if (!ensureDirectory()) return false;
  {
    SpiLock lock(pdMS_TO_TICKS(200));
    if (!lock.ok()) return false;
    File tmp = SD.open(TMP_PATH, FILE_WRITE);
    if (!tmp) return false;
    File source = SD.open(PATH, FILE_READ);
    if (!source) {
      tmp.close();
      SD.remove(TMP_PATH);
      return false;
    }
    uint32_t newOffset = 0;
    for (size_t i = 0; i < count_; ++i) {
      if (!entries_[i].valid) continue;
      DiskRecord record{};
      size_t ignoredSize = 0;
      if (!readDiskRecord(source, entries_[i].offset, record, ignoredSize)) {
        source.close();
        tmp.close();
        SD.remove(TMP_PATH);
        return false;
      }
      record.version = VERSION;
      record.source = entries_[i].source;
      record.priority = entries_[i].priority;
      record.sourceSequence = entries_[i].sourceSequence;
      record.flags = flagsFor(entries_[i].requiredMask, entries_[i].deliveredMask);
      record.crc32 = crc32(reinterpret_cast<const uint8_t*>(&record),
                            offsetof(DiskRecord, crc32));
      if (tmp.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
        source.close();
        tmp.close();
        SD.remove(TMP_PATH);
        return false;
      }
      entries_[i].offset = newOffset;
      entries_[i].recordSize = sizeof(record);
      newOffset += sizeof(record);
    }
    source.close();
    tmp.flush();
    tmp.close();
    if (SD.exists(PATH)) SD.remove(PATH);
    if (!SD.rename(TMP_PATH, PATH)) {
      SD.remove(TMP_PATH);
      return false;
    }
  }
  return true;
}
bool SensorSpool::evictOldest() {
  if (count_ == 0) return false;
  size_t selected = 0;
  for (size_t i = 1; i < count_; ++i) {
    if (entries_[i].priority < entries_[selected].priority ||
        (entries_[i].priority == entries_[selected].priority &&
         entries_[i].recordId < entries_[selected].recordId)) {
      selected = i;
    }
  }
  for (size_t i = selected + 1; i < count_; ++i) entries_[i - 1] = entries_[i];
  entries_[count_ - 1] = {};
  --count_;
  ++evictions_;
  return true;
}

bool SensorSpool::begin() {
  ready_ = false;
  if (!storage.ready()) return false;
  if (!ensureDirectory()) return false;
  if (!scan()) return false;
  if (SD.exists(PATH)) {
    File file = SD.open(PATH, FILE_READ);
    const size_t bytes = file ? static_cast<size_t>(file.size()) : 0;
    if (file) file.close();
    if (bytes > MAX_BYTES && !compact()) return false;
  }
  ready_ = true;
  return true;
}

bool SensorSpool::append(const SensorReader::SensorSample& sample, uint8_t requiredMask,
                           uint8_t source, uint8_t priority, uint32_t sourceSequence,
                           uint8_t schemaVersion, uint32_t firmwareVersion) {
  if (!ready_ || requiredMask == 0 || sample.nodeId == 0 || sample.sensorId == 0 ||
      source > REMOTE_LORA || !std::isfinite(sample.value)) {
    ++drops_;
    return false;
  }
  // Preserve provenance metadata in the durable record; legacy records remain readable.
  const uint32_t id = sampleId(sample);
  for (size_t i = 0; i < count_; ++i)
    if (entries_[i].valid && entries_[i].sampleId == id) return true;

  bool compacted = false;
  if (count_ >= MAX_RECORDS) {
    if (!evictOldest() || !compact()) {
      ++drops_;
      return false;
    }
    compacted = true;
  }

  DiskRecord record{};
  record.type = TYPE_DATA;
  record.flags = flagsFor(requiredMask, 0);
  record.source = source;
  record.recordId = nextRecordId_++;
  record.sampleId = id;
  record.payloadLen = sizeof(SensorReader::SensorSample);
  record.priority = priority;
  record.sourceSequence = sourceSequence;
  record.schemaVersion = schemaVersion;
  record.firmwareVersion = firmwareVersion;
  record.sample = sample;

  size_t currentBytes = 0;
  {
    SpiLock lock(pdMS_TO_TICKS(100));
    if (lock.ok()) {
      File file = SD.open(PATH, FILE_READ);
      if (file) { currentBytes = static_cast<size_t>(file.size()); file.close(); }
    }
  }
  if (compacted) currentBytes = count_ * sizeof(DiskRecord);

  if (currentBytes + sizeof(DiskRecord) > MAX_BYTES) {
    if (!evictOldest() || !compact()) {
      ++drops_;
      return false;
    }
    currentBytes = count_ * sizeof(DiskRecord);
  }
  if (!appendRecord(record)) {
    ++drops_;
    return false;
  }

  IndexEntry& entry = entries_[count_++];
  entry = {};
  entry.recordId = record.recordId;
  entry.sampleId = id;
  entry.offset = static_cast<uint32_t>(currentBytes);
  entry.requiredMask = requiredMask;
  entry.deliveredMask = 0;
  entry.source = source;
  entry.priority = priority;
  entry.sourceSequence = sourceSequence;
  entry.recordSize = sizeof(DiskRecord);
  entry.valid = true;
  return true;
}

bool SensorSpool::peek(Pending& out) const {
  if (!ready_ || count_ == 0) return false;
  const IndexEntry& entry = entries_[0];
  if (!entry.valid) return false;

  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return false;
  File file = SD.open(PATH, FILE_READ);
  if (!file || !file.seek(entry.offset)) {
    if (file) file.close();
    return false;
  }
  DiskRecord record{};
  size_t ignoredSize = 0;
  const bool ok = readDiskRecord(file, entry.offset, record, ignoredSize);
  file.close();
  if (!ok || record.type != TYPE_DATA || record.sampleId != entry.sampleId) return false;
  out.sample = record.sample;
  out.sampleId = entry.sampleId;
  out.requiredMask = entry.requiredMask;
  out.deliveredMask = entry.deliveredMask;
  out.source = entry.source;
  out.priority = entry.priority;
  out.sourceSequence = entry.sourceSequence;
  out.schemaVersion = record.schemaVersion;
  out.firmwareVersion = record.firmwareVersion;
  return true;
}

bool SensorSpool::markDelivered(uint32_t id, uint8_t delivery) {
  if (!ready_ || id == 0 || (delivery & (DELIVERY_MQTT | DELIVERY_LORA)) == 0) return false;
  for (size_t i = 0; i < count_; ++i) {
    if (!entries_[i].valid || entries_[i].sampleId != id) continue;
    const uint8_t delivered = static_cast<uint8_t>(
        entries_[i].deliveredMask | (delivery & (DELIVERY_MQTT | DELIVERY_LORA)));
    DiskRecord ack{};
    ack.type = TYPE_ACK;
    ack.flags = flagsFor(entries_[i].requiredMask, delivered);
    ack.source = entries_[i].source;
    ack.priority = entries_[i].priority;
    ack.sourceSequence = entries_[i].sourceSequence;
    ack.recordId = nextRecordId_++;
    ack.sampleId = id;
    ack.payloadLen = 0;
    if (!appendRecord(ack)) return false;
    entries_[i].deliveredMask = delivered;
    if (delivered == entries_[i].requiredMask) {
      entries_[i] = entries_[count_ - 1];
      entries_[count_ - 1] = {};
      --count_;
    }
    return true;
  }
  return false;
}

bool SensorSpool::clear() {
  if (!ready_) return false;
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return false;
  const bool ok = !SD.exists(PATH) || SD.remove(PATH);
  if (ok) {
    count_ = 0;
    nextRecordId_ = 1;
  }
  return ok;
}

bool SensorSpool::flush() {
  if (!ready_) return false;
  // Records are opened, written, flushed and closed synchronously. Re-opening
  // here verifies that the spool file remains accessible before planned reboot.
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return false;
  File file = SD.open(PATH, FILE_READ);
  if (!file) return true;
  file.close();
  return true;
}

String SensorSpool::statusJson() const {
  String out = "{\"ready\":";
  out += ready_ ? "true" : "false";
  out += ",\"depth\":" + String(static_cast<unsigned>(count_));
  out += ",\"maxRecords\":" + String(static_cast<unsigned>(MAX_RECORDS));
  out += ",\"maxBytes\":" + String(static_cast<unsigned long>(MAX_BYTES));
  out += ",\"evictions\":" + String(evictions_);
  out += ",\"drops\":" + String(drops_);
  out += ",\"recovered\":" + String(recovered_);
  out += "}";
  return out;
}
