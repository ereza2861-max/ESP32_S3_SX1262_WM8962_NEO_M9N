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
  return pps_.begin(Board::GNSS_PPS);
}

void GnssManager::task() {
  while (serial_.available() > 0) {
    gps_.encode(static_cast<char>(serial_.read()));
  }

  static bool hasLastTrackLogLocation = false;
  static double lastTrackLogLat = 0.0;
  static double lastTrackLogLon = 0.0;
  constexpr double TRACK_LOG_DISTANCE_DEG = 0.0001;
  RuntimeConfig runtimeConfig;
  if (!configSnapshot(runtimeConfig)) return;
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
    const uint32_t ppsEdgeUs = pps_.lastEdgeUs();
    gState.gps.ppsLastEdgeUs = ppsEdgeUs;
    gState.gps.ppsValid = ppsEdgeUs != 0U &&
        static_cast<uint32_t>(micros() - ppsEdgeUs) <= Config::GNSS_PPS_VALID_US;
    const double parsedLat = gps_.location.lat();
    const double parsedLon = gps_.location.lng();
    if (gps_.location.isValid() && gps_.location.age() < Config::GNSS_STALE_MS &&
        isfinite(parsedLat) && isfinite(parsedLon) &&
        parsedLat >= -90.0 && parsedLat <= 90.0 &&
        parsedLon >= -180.0 && parsedLon <= 180.0) {
      gState.gps.valid = true;
      gState.gps.lat = parsedLat;
      gState.gps.lon = parsedLon;
      gState.gps.alt = gps_.altitude.isValid() ? gps_.altitude.meters() : 0.0;
      const uint32_t rawSatellites =
          gps_.satellites.isValid() ? gps_.satellites.value() : 0;
      // NMEA satellite counts are small; reject corrupt parser values instead
      // of allowing an implausible value to propagate into telemetry.
      gState.gps.satellites = min<uint32_t>(rawSatellites, 64U);
      gState.gps.hdop_x10 =
          gps_.hdop.isValid() ? static_cast<uint32_t>(gps_.hdop.value() / 10) : 0;
      gState.gps.lastFixMs = now;
      const int year = gps_.date.year();
      const int month = gps_.date.month();
      const int day = gps_.date.day();
      const int hour = gps_.time.hour();
      const int minute = gps_.time.minute();
      const int second = gps_.time.second();
      if (gps_.date.isValid() && gps_.time.isValid() &&
          year >= 2000 && year <= 2199 &&
          month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
          hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 &&
          second >= 0 && second <= 59) {
        const uint64_t days = daysFromCivil(year, month, day);
        gState.gps.timeValid = true;
        gState.gps.utcEpoch = days * 86400ULL +
            static_cast<uint64_t>(gps_.time.hour()) * 3600ULL +
            static_cast<uint64_t>(gps_.time.minute()) * 60ULL +
            static_cast<uint64_t>(gps_.time.second());
        epoch = gState.gps.utcEpoch;
        const time_t systemNow = time(nullptr);
        syncSystemTime = epoch > 1700000000ULL &&
            (lastSyncMs_ == 0U || systemNow < 1700000000 ||
             llabs(static_cast<long long>(systemNow) - static_cast<long long>(epoch)) > 2 ||
             now - lastSyncMs_ >=
                static_cast<uint64_t>(runtimeConfig.wakePeriodSec) * 1000ULL);
      }
      const double currentLat = gState.gps.lat;
      const double currentLon = gState.gps.lon;
      const bool locationMoved =
          !hasLastTrackLogLocation ||
          fabs(currentLat - lastTrackLogLat) >= TRACK_LOG_DISTANCE_DEG ||
          fabs(currentLon - lastTrackLogLon) >= TRACK_LOG_DISTANCE_DEG;
      if (locationMoved) {
        hasLastTrackLogLocation = true;
        lastTrackLogLat = currentLat;
        lastTrackLogLon = currentLon;
        logFix = true;
        lat = currentLat; lon = currentLon; alt = gState.gps.alt; sats = gState.gps.satellites;
        epoch = gState.gps.utcEpoch; timeValid = gState.gps.timeValid;
      }
    } else if (now - gState.gps.lastFixMs > Config::GNSS_STALE_MS) {
      gState.gps.valid = false;
    }
  }

  if (syncSystemTime) {
    const uint32_t now = millis();
    if (epoch == lastSyncEpoch_ && now - lastSyncMs_ < 2000U) {
      syncSystemTime = false;
    } else {
      // Keep the state snapshot and system-clock update in one StateLock
      // critical section. This prevents a concurrent GPS update from making
      // the computed epoch stale between validation and settimeofday().
      StateLock lock(gState);
      if (lock.ok() && gState.gps.timeValid && gState.gps.utcEpoch == epoch) {
        struct timeval tv{static_cast<time_t>(epoch), 0};
        const int rc = settimeofday(&tv, nullptr);
        if (rc == 0) {
          lastSyncEpoch_ = epoch;
          lastSyncMs_ = now;
          gState.lastError = "";
        } else {
          gState.lastError = "GNSS system time sync failed";
        }
      }
    }
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
