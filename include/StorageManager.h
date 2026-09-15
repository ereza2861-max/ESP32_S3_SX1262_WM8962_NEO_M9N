#pragma once
#include <Arduino.h>

class StorageManager {
public:
  bool begin();
  bool ready() const { return ready_; }
  String listJson(const String& dir);
  bool removeFile(const String& path);
  bool renameFile(const String& from, const String& to);
  bool isManagedAudioPath(const String& path) const;
  bool prepareRecordingSpace(uint32_t requiredBytes);
  bool isSafePath(const String& path) const;
  bool checksumFile(const String& path, uint32_t& crc, uint64_t& size);
  bool sha256File(const String& path, String& digest, uint64_t& size);
  bool exportGzip(const String& path, const String& outPath);
  uint64_t totalBytes() const;
  uint64_t usedBytes() const;
  // Returns JSON array of TRACK.CSV points filtered by epoch and capped by limit.
  String readTrackCsv(uint64_t fromEpoch, uint64_t toEpoch, size_t limit);
  String readTrackCsvSimplified(uint64_t fromEpoch, uint64_t toEpoch, size_t limit,
                                double epsilonMeters);
  uint32_t gzipStalls() const { return gzipStalls_; }
private:
  bool ready_ = false;
  uint32_t gzipStalls_ = 0;
};
