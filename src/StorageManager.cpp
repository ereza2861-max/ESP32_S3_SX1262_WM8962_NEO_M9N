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
  File root = SD.open(dir);
  if (!root || !root.isDirectory()) return "[]";

  String out = "[";
  bool first = true;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (!first) out += ",";
    first = false;

    String n = f.name();
    n.replace("\\", "\\\\");
    n.replace("\"", "\\\"");
    out += "{\"name\":\"" + n + "\",\"size\":" +
           String(static_cast<uint32_t>(f.size())) + "}";
    f.close();
  }
  root.close();
  out += "]";
  return out;
}

bool StorageManager::removeFile(const String& path) {
  return isSafePath(path) && SD.remove(path);
}
