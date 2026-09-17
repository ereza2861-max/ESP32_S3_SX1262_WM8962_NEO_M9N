#include "Telemetry.h"
#include "AppState.h"
#include <math.h>

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

String makeLoRaWANUplinkJson() {
  StateLock lock(gState);
  if (!lock.ok() || !gState.gps.valid) return "";
  const int32_t latE5 = static_cast<int32_t>(lround(gState.gps.lat * 100000.0));
  const int32_t lonE5 = static_cast<int32_t>(lround(gState.gps.lon * 100000.0));
  const int32_t altM = static_cast<int32_t>(lround(gState.gps.alt));
  const int32_t battery = gState.batteryPercent < 0 ? 0 : gState.batteryPercent;
  const uint32_t epoch = gState.gps.timeValid
      ? static_cast<uint32_t>(gState.gps.utcEpoch)
      : 0U;
  const uint8_t sos = gState.sos ? 1U : 0U;
  // Compact JSON array: [latE5, lonE5, altM, satellites, battery%, sos, epoch].
  // This keeps normal GPS values below the 51-byte DR0 application budget.
  return "[" + String(latE5) + "," + String(lonE5) + "," +
         String(altM) + "," + String(gState.gps.satellites) + "," +
         String(battery) + "," + String(sos) + "," + String(epoch) + "]";
}
