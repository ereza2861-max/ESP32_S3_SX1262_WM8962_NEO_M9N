#include "GnssManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "StorageManager.h"
#include <SD.h>
#include <sys/time.h>
#include <stdlib.h>
extern StorageManager storage;

namespace {
uint64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<uint64_t>(era * 146097 + static_cast<int64_t>(doe) - 719468);
}
}
bool GnssManager::begin() {
  serial_.begin(Board::GNSS_BAUD, SERIAL_8N1, Board::GNSS_RX, Board::GNSS_TX);
  return true;
}

void GnssManager::task() {
  while (serial_.available() > 0) {
    gps_.encode(static_cast<char>(serial_.read()));
  }

  static uint32_t lastTrackLogMs = 0;
  const uint32_t now = millis();
  bool logFix = false;
  double lat = 0.0, lon = 0.0, alt = 0.0;
  uint32_t sats = 0;
  uint64_t epoch = 0;
  bool timeValid = false;
  bool syncSystemTime = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return;
    if (gps_.location.isValid() && gps_.location.age() < Config::GNSS_STALE_MS) {
      gState.gps.valid = true;
      gState.gps.lat = gps_.location.lat();
      gState.gps.lon = gps_.location.lng();
      gState.gps.alt = gps_.altitude.isValid() ? gps_.altitude.meters() : 0.0;
      gState.gps.satellites =
          gps_.satellites.isValid() ? gps_.satellites.value() : 0;
      gState.gps.hdop_x10 =
          gps_.hdop.isValid() ? static_cast<uint32_t>(gps_.hdop.value() / 10) : 0;
      gState.gps.lastFixMs = now;
      if (gps_.date.isValid() && gps_.time.isValid()) {
        const uint64_t days = daysFromCivil(gps_.date.year(), gps_.date.month(), gps_.date.day());
        gState.gps.timeValid = true;
        gState.gps.utcEpoch = days * 86400ULL +
            static_cast<uint64_t>(gps_.time.hour()) * 3600ULL +
            static_cast<uint64_t>(gps_.time.minute()) * 60ULL +
            static_cast<uint64_t>(gps_.time.second());
        epoch = gState.gps.utcEpoch;
        const time_t systemNow = time(nullptr);
        syncSystemTime = epoch > 1700000000ULL &&
            (systemNow < 1700000000 || llabs(static_cast<long long>(systemNow) - static_cast<long long>(epoch)) > 2);
      }
      if (now - lastTrackLogMs >= Config::TRACK_LOG_PERIOD_MS) {
        lastTrackLogMs = now;
        logFix = true;
        lat = gState.gps.lat; lon = gState.gps.lon; alt = gState.gps.alt; sats = gState.gps.satellites;
        epoch = gState.gps.utcEpoch; timeValid = gState.gps.timeValid;
      }
    } else if (now - gState.gps.lastFixMs > Config::GNSS_STALE_MS) {
      gState.gps.valid = false;
    }
  }

  if (syncSystemTime) {
    struct timeval tv{static_cast<time_t>(epoch), 0};
    (void)settimeofday(&tv, nullptr);
  }

  if (logFix && storage.ready()) {
    SpiLock spiLock(pdMS_TO_TICKS(50));
    if (spiLock.ok()) {
      if (!SD.exists("/TRACK") && SD.mkdir("/TRACK")) {}
      File f = SD.open("/TRACK/TRACK.CSV", FILE_APPEND);
      if (f && f.size() >= Config::TRACK_MAX_BYTES) {
        f.close();
        for (int i = Config::TRACK_ROTATIONS; i >= 1; --i) {
          const String src = i == 1 ? "/TRACK/TRACK.CSV" :
                             "/TRACK/TRACK." + String(i - 1) + ".CSV";
          const String dst = "/TRACK/TRACK." + String(i) + ".CSV";
          if (SD.exists(dst)) SD.remove(dst);
          if (SD.exists(src)) SD.rename(src, dst);
        }
        f = SD.open("/TRACK/TRACK.CSV", FILE_APPEND);
      }
      if (f) {
        if (f.size() == 0) f.println("epoch_s,millis,lat,lon,alt_m,sat");
        f.printf("%llu,%lu,%.6f,%.6f,%.1f,%lu\n",
                 static_cast<unsigned long long>(timeValid ? epoch : 0),
                 static_cast<unsigned long>(now), lat, lon, alt,
                 static_cast<unsigned long>(sats));
        f.close();
      }
    }
  }
}
