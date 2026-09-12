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
private:
  bool ready_ = false;
};
