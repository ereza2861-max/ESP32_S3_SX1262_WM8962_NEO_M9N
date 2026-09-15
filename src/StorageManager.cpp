#include "StorageManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include <SD.h>
#include <SPI.h>
#include <mbedtls/sha256.h>
#include <cstdio>
#include <cmath>
#include <zlib.h>

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
    out += "{\"name\":\"" + escaped + "\",\"dir\":" + String(f.isDirectory() ? "true" : "false") + ",\"size\":" +
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
  String protectedRecordingFile;
  {
    StateLock stateLock(gState);
    if (!stateLock.ok()) return false;
    protectedRecordingFile = gState.lastAudioFile;
  }
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;
  if (!SD.exists("/REC") && !SD.mkdir("/REC")) return false;
  const uint64_t total = SD.totalBytes();
  const uint64_t used = SD.usedBytes();
  if (total == 0 || used > total) return false;

  const uint64_t maxUsed = min<uint64_t>(total, Config::RECORD_MAX_TOTAL_BYTES);
  if (requiredBytes > maxUsed ||
      static_cast<uint64_t>(Config::RECORD_MIN_FREE_BYTES) + requiredBytes > total)
    return false;
  uint64_t effectiveUsed = used;
  while (effectiveUsed + requiredBytes > maxUsed ||
         total - effectiveUsed < Config::RECORD_MIN_FREE_BYTES + requiredBytes) {
    File dir = SD.open("/REC");
    if (!dir || !dir.isDirectory()) return false;

    String oldest;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory()) {
        String name = f.name();
        // Arduino-ESP32 FS implementations are not consistent here: some
        // return the full path while others return only the entry name.
        // Normalize before calling SD.remove(), otherwise cleanup can reject
        // every candidate and recording fails exactly when storage is low.
        if (!name.startsWith("/")) name = "/REC/" + name;
        if (name.startsWith("/REC/") && name.substring(name.lastIndexOf(".")).equalsIgnoreCase(".WAV") &&
            name.length() > 5) {
          const bool protectedFile = !protectedRecordingFile.isEmpty() &&
                                      name == protectedRecordingFile;
          if (!protectedFile && (oldest.isEmpty() || name.compareTo(oldest) < 0))
            oldest = name;
        }
      }
      f.close();
    }
    dir.close();
    if (oldest.isEmpty()) return false;
    if (!SD.remove(oldest)) return false;
    // FAT allocation is cluster-based, so subtracting the logical file size
    // from SD.usedBytes() can underestimate the actual space reclaimed.
    const uint64_t refreshedUsed = SD.usedBytes();
    if (refreshedUsed > total) return false;
    effectiveUsed = refreshedUsed;
  }
  return true;
}


uint64_t StorageManager::totalBytes() const {
  if (!ready_) return 0;
  SpiLock spiLock(pdMS_TO_TICKS(100));
  return spiLock.ok() ? SD.totalBytes() : 0;
}

uint64_t StorageManager::usedBytes() const {
  if (!ready_) return 0;
  SpiLock spiLock(pdMS_TO_TICKS(100));
  return spiLock.ok() ? SD.usedBytes() : 0;
}

bool StorageManager::checksumFile(const String& path, uint32_t& crc, uint64_t& size) {
  if (!isSafePath(path)) return false;
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }
  crc = 0xFFFFFFFFU;
  size = 0;
  uint8_t buf[512];
  while (f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    if (n == 0) { f.close(); return false; }
    size += n;
    for (size_t i = 0; i < n; ++i) {
      crc ^= buf[i];
      for (uint8_t b = 0; b < 8; ++b)
        crc = (crc >> 1) ^ (0xEDB88320U & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U))));
    }
  }
  f.close();
  crc ^= 0xFFFFFFFFU;
  return true;
}


bool StorageManager::sha256File(const String& path, String& digest, uint64_t& size) {
  digest = String();
  size = 0;
  if (!isSafePath(path)) return false;
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return false;
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
  uint8_t buf[1024];
  while (ok && f.available()) {
    const size_t n = f.read(buf, sizeof(buf));
    if (n == 0) { ok = false; break; }
    size += n;
    ok = mbedtls_sha256_update(&ctx, buf, n) == 0;
  }
  uint8_t hash[32] = {};
  if (ok) ok = mbedtls_sha256_finish(&ctx, hash) == 0;
  mbedtls_sha256_free(&ctx);
  f.close();
  if (!ok) return false;
  const char* digits = "0123456789abcdef";
  digest.reserve(64);
  for (uint8_t b : hash) { digest += digits[b >> 4]; digest += digits[b & 0x0F]; }
  return true;
}


bool StorageManager::exportGzip(const String& path, const String& outPath) {
  if (!isSafePath(path) || !isSafePath(outPath) || !ready_ || path == outPath)
    return false;
  SpiLock spiLock(pdMS_TO_TICKS(500));
  if (!spiLock.ok()) return false;
  File in = SD.open(path, FILE_READ);
  if (!in || in.isDirectory()) {
    if (in) in.close();
    return false;
  }
  if (SD.exists(outPath)) SD.remove(outPath);
  File out = SD.open(outPath, FILE_WRITE);
  if (!out) { in.close(); return false; }

  z_stream zs{};
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    in.close(); out.close(); return false;
  }
  uint8_t inBuf[1024], outBuf[2048];
  bool ok = true;
  int flush = Z_NO_FLUSH;
  uint8_t zeroReads = 0;
  while (ok) {
    const size_t got = in.read(inBuf, sizeof(inBuf));
    if (!got) {
      ++zeroReads;
      if (in.available() == 0) flush = Z_FINISH;
      else if (zeroReads >= 2) {
        ++gzipStalls_;
        StateLock lock(gState);
        if (lock.ok()) gState.lastError = "Gzip export stalled";
        ok = false;
        break;
      }
    } else {
      zeroReads = 0;
    }
    zs.next_in = inBuf;
    zs.avail_in = static_cast<uInt>(got);
    if (got) flush = in.available() ? Z_NO_FLUSH : Z_FINISH;
    do {
      zs.next_out = outBuf;
      zs.avail_out = sizeof(outBuf);
      const int rc = deflate(&zs, flush);
      if (rc != Z_OK && rc != Z_STREAM_END) { ok = false; break; }
      const size_t produced = sizeof(outBuf) - zs.avail_out;
      if (produced && out.write(outBuf, produced) != produced) { ok = false; break; }
      if (rc == Z_STREAM_END) break;
    } while (zs.avail_in || flush == Z_FINISH);
    if (!got && flush == Z_FINISH) break;
  }
  deflateEnd(&zs);
  in.close();
  out.close();
  if (!ok) SD.remove(outPath);
  return ok;
}

String StorageManager::readTrackCsv(uint64_t fromEpoch, uint64_t toEpoch, size_t limit) {
  if (!ready_ || limit == 0 || toEpoch < fromEpoch) return "[]";
  limit = min<size_t>(limit, 5000);
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return "[]";
  File f = SD.open("/TRACK/TRACK.CSV", FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return "[]";
  }

  String out = "[";
  bool first = true;
  size_t emitted = 0;
  while (f.available() && emitted < limit) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("epoch_s")) continue;

    unsigned long long epoch = 0;
    unsigned long ms = 0, sat = 0;
    double lat = 0.0, lon = 0.0, alt = 0.0;
    if (sscanf(line.c_str(), "%llu,%lu,%lf,%lf,%lf,%lu",
               &epoch, &ms, &lat, &lon, &alt, &sat) != 6)
      continue;
    if (epoch < fromEpoch || epoch > toEpoch) continue;
    if (!isfinite(lat) || !isfinite(lon) || !isfinite(alt) ||
        lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
      continue;

    if (!first) out += ",";
    first = false;
    out += "{\"epoch\":" + String(epoch) +
           ",\"millis\":" + String(ms) +
           ",\"lat\":" + String(lat, 6) +
           ",\"lon\":" + String(lon, 6) +
           ",\"alt\":" + String(alt, 1) +
           ",\"sat\":" + String(sat) + "}";
    ++emitted;
  }
  f.close();
  out += "]";
  return out;
}


String StorageManager::readTrackCsvSimplified(uint64_t fromEpoch, uint64_t toEpoch,
                                              size_t limit, double epsilonMeters) {
  if (!ready_ || limit == 0 || toEpoch < fromEpoch || !isfinite(epsilonMeters) ||
      epsilonMeters <= 0.0) return "[]";
  struct Point { uint64_t epoch; uint32_t ms; double lat; double lon; double alt; uint32_t sat; };
  static constexpr size_t MAX_POINTS = 5000;
  Point points[MAX_POINTS] = {};
  size_t n = 0;

  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) return "[]";
  File f = SD.open("/TRACK/TRACK.CSV", FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return "[]";
  }
  while (f.available() && n < min(limit, MAX_POINTS)) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("epoch_s")) continue;
    unsigned long long epoch = 0;
    unsigned long ms = 0, sat = 0;
    double lat = 0.0, lon = 0.0, alt = 0.0;
    if (sscanf(line.c_str(), "%llu,%lu,%lf,%lf,%lf,%lu",
               &epoch, &ms, &lat, &lon, &alt, &sat) != 6) continue;
    if (epoch < fromEpoch || epoch > toEpoch ||
        !isfinite(lat) || !isfinite(lon) ||
        lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) continue;
    points[n++] = {static_cast<uint64_t>(epoch), static_cast<uint32_t>(ms),
                   lat, lon, alt, static_cast<uint32_t>(sat)};
  }
  f.close();
  if (n <= 2) {
    String out = "[";
    for (size_t i = 0; i < n; ++i) {
      if (i) out += ",";
      out += "{\"epoch\":" + String(points[i].epoch) +
             ",\"millis\":" + String(points[i].ms) +
             ",\"lat\":" + String(points[i].lat, 6) +
             ",\"lon\":" + String(points[i].lon, 6) +
             ",\"alt\":" + String(points[i].alt, 1) +
             ",\"sat\":" + String(points[i].sat) + "}";
    }
    out += "]";
    return out;
  }

  bool keep[MAX_POINTS] = {};
  keep[0] = keep[n - 1] = true;
  struct Range { size_t a, b; };
  Range stack[MAX_POINTS];
  size_t sp = 0;
  stack[sp++] = {0, n - 1};
  const double rad = 0.017453292519943295;
  auto xy = [&](const Point& p, double refLat, double& x, double& y) {
    x = p.lon * cos(refLat * rad) * 111320.0;
    y = p.lat * 110540.0;
  };
  while (sp) {
    const Range r = stack[--sp];
    if (r.b <= r.a + 1) continue;
    const double refLat = points[r.a].lat * rad;
    double ax, ay, bx, by;
    xy(points[r.a], points[r.a].lat, ax, ay);
    xy(points[r.b], points[r.a].lat, bx, by);
    const double dx = bx - ax, dy = by - ay;
    double maxDist = -1.0;
    size_t index = r.a;
    for (size_t i = r.a + 1; i < r.b; ++i) {
      double px, py;
      xy(points[i], points[r.a].lat, px, py);
      double dist;
      const double denom = dx * dx + dy * dy;
      if (denom < 1e-9) {
        dist = hypot(px - ax, py - ay);
      } else {
        const double t = constrain((px - ax) * dx + (py - ay) * dy, 0.0, 1.0);
        dist = hypot(px - (ax + t * dx), py - (ay + t * dy));
      }
      if (dist > maxDist) { maxDist = dist; index = i; }
    }
    if (maxDist > epsilonMeters) {
      keep[index] = true;
      if (index > r.a + 1) stack[sp++] = {r.a, index};
      if (r.b > index + 1) stack[sp++] = {index, r.b};
    }
  }

  String out = "[";
  bool first = true;
  for (size_t i = 0; i < n; ++i) {
    if (!keep[i]) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"epoch\":" + String(points[i].epoch) +
           ",\"millis\":" + String(points[i].ms) +
           ",\"lat\":" + String(points[i].lat, 6) +
           ",\"lon\":" + String(points[i].lon, 6) +
           ",\"alt\":" + String(points[i].alt, 1) +
           ",\"sat\":" + String(points[i].sat) + "}";
  }
  out += "]";
  return out;
}
