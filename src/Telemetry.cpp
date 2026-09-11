#include "Telemetry.h"
#include "AppState.h"

String makePositionTelemetry() {
  StateLock lock(gState);
  if (!lock.ok() || !gState.gps.valid) return "ERR,NOFIX";
  return "POS," + String(gState.gps.lat, 6) + "," +
         String(gState.gps.lon, 6) + ",ALT=" +
         String(gState.gps.alt, 1) + ",SAT=" +
         String(gState.gps.satellites);
}

String makeAprsLikePosition() {
  StateLock lock(gState);
  if (!lock.ok() || !gState.gps.valid) return "";
  return "POS," + String(gState.gps.lat, 6) + "," +
         String(gState.gps.lon, 6) + ",ALT=" + String(gState.gps.alt, 1);
}
