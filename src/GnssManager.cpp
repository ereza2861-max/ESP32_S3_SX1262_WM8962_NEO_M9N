#include "GnssManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"

bool GnssManager::begin() {
  serial_.begin(Board::GNSS_BAUD, SERIAL_8N1, Board::GNSS_RX, Board::GNSS_TX);
  return true;
}

void GnssManager::task() {
  while (serial_.available() > 0) {
    gps_.encode(static_cast<char>(serial_.read()));
  }

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
    gState.gps.lastFixMs = millis();
  } else if (millis() - gState.gps.lastFixMs > Config::GNSS_STALE_MS) {
    gState.gps.valid = false;
  }
}
