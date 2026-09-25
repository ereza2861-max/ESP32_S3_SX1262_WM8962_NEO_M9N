#pragma once

#include <Arduino.h>
#include "ProfileConfig.h"
#include "SensorDriverRegistry.h"
#include "SensorRegistry.h"

// ProfileSensors owns the per-profile sensor roster: which drivers are
// instantiated, on which interface, with which stable sensor ID.
//
// The current implementation contains source-level rosters for all four
// profiles. Placeholder drivers remain stale/invalid rather than fabricating
// measurements when a hardware-specific protocol is not implemented.
//
// Design constraints (from artifact-goal.md):
//   - No hidden architectural decisions: every driver type used here is
//     declared in ProfileConfig::DriverType and validated by
//     SensorDriverRegistry::validConfig.
//   - No fabricated sensor models: a sensor is only registered if its driver
//     can actually return a value. Sensors that require a UART/SPI protocol
//     parser that STEP 3 does not implement are declared as placeholder
//     entries with QUALITY_STALE, not as fake readings.
//   - Stable IDs: each sensor ID is derived from the profile base so that
//     identity survives reboots and NVS reloads.

class ProfileSensors {
public:
  // Configures the driver roster for the given profile and registers each
  // sensor with the supplied SensorRegistry. Returns true only if the roster
  // was fully configured. Partial success is possible and reported via
  // registeredCount().
  bool begin(ProfileConfig::Profile profile,
             SensorDriverRegistry& drivers,
             SensorRegistry& registry);

  // Number of sensors actually registered by the last begin() call.
  size_t registeredCount() const { return registeredCount_; }

  // Number of sensors the profile intended to register (may exceed
  // registeredCount() when a driver is unavailable). Used by STEP 8 to log a
  // clear "partial roster" warning.
  size_t expectedCount() const { return expectedCount_; }

private:
  bool registerIslandSea(SensorDriverRegistry& drivers,
                         SensorRegistry& registry);
  bool registerTropicalForest(SensorDriverRegistry& drivers, SensorRegistry& registry);
  bool registerVolcanicMountain(SensorDriverRegistry& drivers, SensorRegistry& registry);
  bool registerSubZeroSnow(SensorDriverRegistry& drivers, SensorRegistry& registry);

  size_t registeredCount_ = 0;
  size_t expectedCount_ = 0;
};


namespace ProfileSensorsContract {
constexpr bool gpio3IsTimeSharedInProfile2() { return true; }
constexpr int gpio3OwnerWhenSamplingAdc() { return -1; }
constexpr int gpio3OwnerWhenSamplingSpi() { return ProfileConfig::PROFILE2_ADXL355_CS_PIN; }
}
