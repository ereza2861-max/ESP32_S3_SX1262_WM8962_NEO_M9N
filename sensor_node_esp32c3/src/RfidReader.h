#pragma once

#include <Arduino.h>
#include "ProfileConfig.h"
#include <cstdint>

// FieldRadio ESP32-C3 Sensor Node — MFRC522 RFID reader wrapper.
//
// STEP 2 scope:
//   - MFRC522 over SPI on the pins locked in ProfileConfig.h.
//   - Non-blocking task() called from loop(); polls at ProfileConfig::RFID_POLL_INTERVAL_MS.
//   - Debounces repeated detection of the same UID so the buzzer does not
//     retrigger on every poll while a tag remains in the field.
//   - Fires a single "tag detected" callback per unique UID appearance.
//   - main.cpp registers the global SensorRegistry descriptor at
//     SENSOR_ID_BASE_RFID (0x00F0) and routes tag events to it.
//   - begin() returning false is NON-FATAL: the node keeps running BLE + sensors.
//
// This class deliberately does NOT include ProfileManager.h to avoid a
// circular dependency with any future buzzer-aware ProfileManager. The buzzer
// hook is provided as a plain function pointer set by main.cpp.

class RfidReader {
public:
  // Called once per unique UID appearance. The uid pointer is valid only for
  // the duration of the call; copy it if you need to retain it.
  using TagCallback = void (*)(const uint8_t* uid, uint8_t uidLength);

  RfidReader() = default;
  ~RfidReader();

  // Initializes SPI and the MFRC522. Returns false if the reader is not
  // detected (no hardware, wrong wiring, or SPI bus conflict). A false return
  // is not fatal; the caller must continue without RFID.
  bool begin();

  // Non-blocking cooperative task. Call from loop(). Polls at
  // ProfileConfig::RFID_POLL_INTERVAL_MS and invokes the tag callback exactly once per new
  // UID. A UID is considered "gone" after ProfileConfig::RFID_UID_FORGET_MS of absence.
  void task();

  bool isReady() const { return ready_; }

  // Returns the most recently detected UID length, or 0 if none.
  uint8_t lastUidLength() const { return lastUidLength_; }

  // Copies the most recent UID into out (must be at least 10 bytes). Returns
  // the UID length, or 0 if no UID has been detected yet.
  uint8_t lastUid(uint8_t* out, uint8_t outCapacity) const;

  void setTagCallback(TagCallback callback) { tagCallback_ = callback; }

private:
  bool ready_ = false;
  uint32_t lastPollMs_ = 0;

  uint8_t lastUid_[ProfileConfig::RFID_MAX_UID_BYTES] = {};
  uint8_t lastUidLength_ = 0;
  uint32_t lastUidSeenMs_ = 0;
  bool uidPresent_ = false;

  TagCallback tagCallback_ = nullptr;
};
