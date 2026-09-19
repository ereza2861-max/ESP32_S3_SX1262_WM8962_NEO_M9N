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
}  // namespace

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

  const size_t recordSize = sizeof(DiskRecord);
  uint8_t raw[sizeof(DiskRecord)] = {};
  size_t validBytes = 0;
  while (file.available()) {
    const size_t start = static_cast<size_t>(file.position());
    const size_t n = file.read(raw, recordSize);
    if (n != recordSize) break;
    DiskRecord record{};
    std::memcpy(&record, raw, sizeof(record));
    if (record.magic != MAGIC || record.version != VERSION ||
        (record.type == TYPE_DATA && record.payloadLen != sizeof(SensorReader::SensorSample)) ||
        (record.type == TYPE_ACK && record.payloadLen != 0) ||
        (record.type != TYPE_DATA && record.type != TYPE_ACK) ||
        record.crc32 != crc32(raw, offsetof(DiskRecord, crc32))) {
      break;
    }
    validBytes = start + recordSize;
    if (record.recordId >= nextRecordId_) nextRecordId_ = record.recordId + 1U;
    if (record.type == TYPE_DATA) {
      if (record.sampleId == 0 || requiredMaskFromFlags(record.flags) == 0) continue;
      if (count_ >= MAX_RECORDS) {
        for (size_t i = 1; i < count_; ++i) entries_[i - 1] = entries_[i];
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
      entry.valid = true;
      if (entry.deliveredMask == entry.requiredMask) --count_;
      else {}
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
    }
  }
  file.close();

  // A torn final record is discarded. All complete records before it remain
  // usable because every record has its own CRC32.
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
      if (!source.seek(entries_[i].offset) ||
          source.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
        source.close();
        tmp.close();
        SD.remove(TMP_PATH);
        return false;
      }
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
  for (size_t i = 1; i < count_; ++i) entries_[i - 1] = entries_[i];
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

bool SensorSpool::append(const SensorReader::SensorSample& sample, uint8_t requiredMask) {
  if (!ready_ || requiredMask == 0 || sample.nodeId == 0 || sample.sensorId == 0 ||
      !std::isfinite(sample.value)) {
    ++drops_;
    return false;
  }
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
  record.recordId = nextRecordId_++;
  record.sampleId = id;
  record.payloadLen = sizeof(SensorReader::SensorSample);
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
  const bool ok = file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.close();
  if (!ok || record.magic != MAGIC || record.version != VERSION ||
      record.type != TYPE_DATA || record.sampleId != entry.sampleId ||
      record.crc32 != crc32(reinterpret_cast<const uint8_t*>(&record), offsetof(DiskRecord, crc32))) {
    return false;
  }
  out.sample = record.sample;
  out.sampleId = entry.sampleId;
  out.requiredMask = entry.requiredMask;
  out.deliveredMask = entry.deliveredMask;
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
