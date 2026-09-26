#pragma once

#include <Arduino.h>
#include "ProfileConfig.h"

// ProfileManager owns three responsibilities:
//   1. Load/save the active profile from the "sensor" NVS namespace.
//   2. Provide a non-blocking buzzer pulse API.
//   3. Run a non-blocking button state machine that reports a 1.5 s long press.
//
// The manager does NOT start Wi-Fi AP, does NOT touch BLE, and does NOT own
// sensor drivers. OtaApManager (OTA) subscribes to the long-press callback.

class ProfileManager {
public:
  using LongPressCallback = void (*)();

  // Loads the persisted runtime profile, configures buzzer + button GPIOs,
  // and falls back to profile 0 if the stored value is invalid.
  bool begin();

  // Non-blocking cooperative task. Call from loop(). Handles button debounce,
  // long-press detection, and buzzer auto-off.
  void task();

  // Triggers a single buzzer pulse of BUZZER_DURATION_MS at BUZZER_FREQ_HZ.
  // Safe to call from RFID detection or any other event handler. A subsequent
  // call while a pulse is active extends the pulse rather than overlapping.
  void buzzerPulse();

  // Returns the currently active runtime profile.
  ProfileConfig::Profile activeProfile() const { return activeProfile_; }

  // Loads the runtime profile from NVS namespace "sensor".
  bool loadProfileFromNvs();

  // Persists the current runtime profile in NVS namespace "sensor".
  bool saveProfileToNvs();

  // Changes and persists the runtime profile.
  bool setProfile(ProfileConfig::Profile profile);

  // Registers the callback fired once per confirmed 1.5 s button long press.
  // The callback runs from task() context; it must not block.
  void setLongPressCallback(LongPressCallback callback) { longPressCallback_ = callback; }

  // True while the physical button is held down (after debounce).
  bool buttonHeld() const { return buttonState_ == ButtonState::Holding ||
                                   buttonState_ == ButtonState::LongFired; }

private:
  enum class ButtonState : uint8_t {
    Idle = 0,
    Debouncing,
    Holding,
    LongFired,
  };

  ProfileConfig::Profile activeProfile_ = ProfileConfig::Profile::IslandSea;

  ButtonState buttonState_ = ButtonState::Idle;
  uint32_t buttonStateSinceMs_ = 0;
  bool lastRawButtonLevel_ = true;  // INPUT_PULLUP: idle = HIGH

  uint32_t buzzerOffAtMs_ = 0;
  bool buzzerActive_ = false;

  LongPressCallback longPressCallback_ = nullptr;
};
