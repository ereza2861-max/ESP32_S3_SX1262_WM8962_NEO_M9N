#include "GnssManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "StorageManager.h"
#include <SD.h>
extern StorageManager storage;

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
      if (now - lastTrackLogMs >= Config::TRACK_LOG_PERIOD_MS) {
        lastTrackLogMs = now;
        logFix = true;
        lat = gState.gps.lat; lon = gState.gps.lon; alt = gState.gps.alt; sats = gState.gps.satellites;
      }
    } else if (now - gState.gps.lastFixMs > Config::GNSS_STALE_MS) {
      gState.gps.valid = false;
    }
  }

  if (logFix && storage.ready()) {
    SpiLock spiLock(pdMS_TO_TICKS(50));
    if (spiLock.ok()) {
      if (!SD.exists("/TRACK") && SD.mkdir("/TRACK")) {}
      File f = SD.open("/TRACK/TRACK.CSV", FILE_APPEND);
      if (f) {
        if (f.size() == 0) f.println("millis,lat,lon,alt_m,sat");
        f.printf("%lu,%.6f,%.6f,%.1f,%lu\n",
                 static_cast<unsigned long>(now), lat, lon, alt,
                 static_cast<unsigned long>(sats));
        f.close();
      }
    }
  }
}
