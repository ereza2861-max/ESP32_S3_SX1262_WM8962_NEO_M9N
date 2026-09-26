#pragma once

#include <Arduino.h>
#include "ProfileConfig.h"

// ProfileManager owns three responsibilities for STEP 1:
//   1. Read the 2-bit production profile selector once at boot and expose the active profile.
//   2. Provide a non-blocking buzzer pulse API.
//   3. Run a non-blocking button state machine that reports a 1.5 s long press.
//
// The manager does NOT start Wi-Fi AP, does NOT touch BLE, and does NOT own
// sensor drivers. OtaApManager (STEP 7) subscribes to the long-press callback.

class ProfileManager {
public:
  using LongPressCallback = void (*)();

  // Reads the profile selector, configures buzzer + button GPIOs, and stores the
  // active profile. Must be called after Serial.begin() and after any
  // provisioning load that does not touch GPIOs.
  bool begin();

  // Non-blocking cooperative task. Call from loop(). Handles button debounce,
  // long-press detection, and buzzer auto-off.
  void task();

  // Triggers a single buzzer pulse of BUZZER_DURATION_MS at BUZZER_FREQ_HZ.
  // Safe to call from RFID detection or any other event handler. A subsequent
  // call while a pulse is active extends the pulse rather than overlapping.
  void buzzerPulse();

  // Returns the profile selected by the static profile selector at begin().
  ProfileConfig::Profile activeProfile() const { return activeProfile_; }

  // Returns the raw 2-bit profile-selector value in [0, 3] for diagnostics.
  uint8_t rawDipValue() const { return rawDipValue_; }

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

  static uint8_t readProfileSelector(int bit0, int bit1);

  ProfileConfig::Profile activeProfile_ = ProfileConfig::Profile::IslandSea;
  uint8_t rawDipValue_ = 0;

  ButtonState buttonState_ = ButtonState::Idle;
  uint32_t buttonStateSinceMs_ = 0;
  bool lastRawButtonLevel_ = true;  // INPUT_PULLUP: idle = HIGH

  uint32_t buzzerOffAtMs_ = 0;
  bool buzzerActive_ = false;

  LongPressCallback longPressCallback_ = nullptr;
};
