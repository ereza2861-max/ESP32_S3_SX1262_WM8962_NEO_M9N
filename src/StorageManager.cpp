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

bool StorageManager::removeFile(const String& path) {
  // The web API is intended to manage recorded WAV files only. Restrict
  // deletion to direct children of /REC so unrelated SD-card content and
  // directories cannot be removed through this endpoint.
  if (!isSafePath(path) || !path.startsWith("/REC/") || path.endsWith("/") ||
      path.lastIndexOf('/') != 4 || !path.endsWith(".WAV"))
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
