#include "StorageManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include <SD.h>
#include <SPI.h>

bool StorageManager::begin() {
  if (!SD.begin(Board::SD_CS, SPI, 20000000U)) {
    ready_ = false;
    return false;
  }
  ready_ = true;
  StateLock lock(gState);
  if (lock.ok()) gState.storageReady = true;
  return true;
}

bool StorageManager::isSafePath(const String& path) const {
  if (!ready_ || path.length() == 0 || path.length() > Config::MAX_PATH)
    return false;
  if (!path.startsWith("/") || path.indexOf("..") >= 0 ||
      path.indexOf('\\') >= 0 || path.indexOf('\0') >= 0)
    return false;
  return true;
}

String StorageManager::listJson(const String& dir) {
  if (!isSafePath(dir)) return "[]";
  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) return "[]";

  File root = SD.open(dir);
  if (!root || !root.isDirectory()) return "[]";

  String out = "[";
  bool first = true;
  size_t count = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (count >= Config::MAX_WEB_FILES) {
      f.close();
      break;
    }
    ++count;
    if (!first) out += ",";
    first = false;

    String n = f.name();
    String escaped;
    escaped.reserve(n.length() + 8);
    for (size_t i = 0; i < n.length(); ++i) {
      const uint8_t c = static_cast<uint8_t>(n[i]);
      if (c == '\\') escaped += "\\\\";
      else if (c == '"') escaped += "\\\"";
      else if (c == '\n') escaped += "\\n";
      else if (c == '\r') escaped += "\\r";
      else if (c == '\t') escaped += "\\t";
      else if (c < 0x20) {
        const char hex[] = "0123456789ABCDEF";
        escaped += "\\u00";
        escaped += hex[(c >> 4) & 0x0F];
        escaped += hex[c & 0x0F];
      } else escaped += static_cast<char>(c);
    }
    out += "{\"name\":\"" + escaped + "\",\"size\":" +
           String(static_cast<uint32_t>(f.size())) + "}";
    f.close();
  }
  root.close();
  out += "]";
  return out;
}


bool StorageManager::isManagedAudioPath(const String& path) const {
  return isSafePath(path) && path.startsWith("/REC/") &&
         path.lastIndexOf('/') == 4 && !path.endsWith("/") &&
         path.length() > 9 && path.substring(path.lastIndexOf(".")).equalsIgnoreCase(".WAV");
}

bool StorageManager::renameFile(const String& from, const String& to) {
  if (!isManagedAudioPath(from) || !isManagedAudioPath(to) || from == to)
    return false;
  {
    StateLock lock(gState);
    if (!lock.ok() || gState.recording ||
        (gState.playing && (from == gState.lastAudioFile || to == gState.lastAudioFile)))
      return false;
  }
  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok() || !SD.exists(from) || SD.exists(to)) return false;
  return SD.rename(from, to);
}

bool StorageManager::removeFile(const String& path) {
  // The web API is intended to manage recorded WAV files only. Restrict
  // deletion to direct children of /REC so unrelated SD-card content and
  // directories cannot be removed through this endpoint.
  if (!isManagedAudioPath(path))
    return false;

  {
    StateLock lock(gState);
    if (!lock.ok() || gState.recording ||
        (gState.playing && path == gState.lastAudioFile))
      return false;
  }

  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) return false;

  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }
  f.close();
  return SD.remove(path);
}

bool StorageManager::prepareRecordingSpace(uint32_t requiredBytes) {
  if (!ready_) return false;
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;
  if (!SD.exists("/REC") && !SD.mkdir("/REC")) return false;
  const uint64_t total = SD.totalBytes();
  const uint64_t used = SD.usedBytes();
  if (total == 0 || used > total) return false;

  const uint64_t maxUsed = min<uint64_t>(total, Config::RECORD_MAX_TOTAL_BYTES);
  uint64_t effectiveUsed = used;
  while (effectiveUsed + requiredBytes > maxUsed ||
         total - effectiveUsed < Config::RECORD_MIN_FREE_BYTES + requiredBytes) {
    File dir = SD.open("/REC");
    if (!dir || !dir.isDirectory()) return false;

    String oldest;
    uint32_t oldestSize = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory()) {
        String name = f.name();
        // Arduino-ESP32 FS implementations are not consistent here: some
        // return the full path while others return only the entry name.
        // Normalize before calling SD.remove(), otherwise cleanup can reject
        // every candidate and recording fails exactly when storage is low.
        if (!name.startsWith("/")) name = "/REC/" + name;
        if (name.startsWith("/REC/") && name.substring(name.lastIndexOf(".")).equalsIgnoreCase(".WAV") &&
            name.length() > 5 &&
            (oldest.isEmpty() || name.compareTo(oldest) < 0)) {
          oldest = name;
          oldestSize = static_cast<uint32_t>(f.size());
        }
      }
      f.close();
    }
    dir.close();
    if (oldest.isEmpty()) return false;
    if (!SD.remove(oldest)) return false;
    effectiveUsed = effectiveUsed > oldestSize ? effectiveUsed - oldestSize : 0;
  }
  return true;
}
