#include "ProfileManager.h"

#include <Arduino.h>

namespace {

// Non-blocking buzzer helper. Uses the ESP-IDF LEDC peripheral via the
// Arduino-ESP32 wrapper. The exact API depends on the core version:
//   - Arduino-ESP32 3.x: ledcAttach(pin, freq, resolution) at begin() once,
//     then ledcWriteTone(pin, freq) and ledcWrite(pin, 0) to silence.
//   - Arduino-ESP32 2.x: ledcSetup(channel, freq, resolution) +
//     ledcAttachPin(pin, channel) at begin() once, then ledcWriteTone(channel,
//     freq) and ledcWrite(channel, 0) to silence.
//
// STEP 1 does NOT guess which core version is in use. The code below uses the
// 3.x API (ledcAttach / ledcWriteTone / ledcWrite on the pin). If you build
// with Arduino-ESP32 2.x, replace the three calls marked [LEDC] with the
// channel-based equivalents.
void buzzerHardwareInit() {
  // [LEDC] Attach the buzzer pin to the LEDC peripheral. Resolution and
  // frequency are set here; ledcWriteTone() only changes the frequency.
  ledcAttach(ProfileConfig::BUZZER_PIN,
             ProfileConfig::BUZZER_FREQ_HZ,
             ProfileConfig::BUZZER_LEDC_RESOLUTION_BITS);
  // Ensure the output starts silent.
  ledcWrite(ProfileConfig::BUZZER_PIN, 0);
}

void buzzerHardwareTone(uint32_t freqHz) {
  // [LEDC] Start a tone at freqHz with 50% duty (2^(bits-1)).
  const uint32_t halfDuty =
      (1UL << (ProfileConfig::BUZZER_LEDC_RESOLUTION_BITS - 1)) - 1UL;
  ledcWriteTone(ProfileConfig::BUZZER_PIN, freqHz);
  ledcWrite(ProfileConfig::BUZZER_PIN, halfDuty);
}

void buzzerHardwareSilence() {
  // [LEDC] Silence the buzzer.
  ledcWriteTone(ProfileConfig::BUZZER_PIN, 0);
  ledcWrite(ProfileConfig::BUZZER_PIN, 0);
}

}  // namespace

uint8_t ProfileManager::decodeDip(int bit0, int bit1) {
  // bit0 is the least-significant profile bit. DIP switches pull to GND when
  // closed (INPUT_PULLUP => closed = LOW = 0). Open = HIGH = 1.
  const uint8_t b0 = (digitalRead(bit0) == LOW) ? 0U : 1U;
  const uint8_t b1 = (digitalRead(bit1) == LOW) ? 0U : 1U;
  return static_cast<uint8_t>(b0 | (b1 << 1));
}

bool ProfileManager::begin() {
  pinMode(ProfileConfig::DIP_BIT0_PIN, INPUT_PULLUP);
  pinMode(ProfileConfig::DIP_BIT1_PIN, INPUT_PULLUP);

  rawDipValue_ = decodeDip(ProfileConfig::DIP_BIT0_PIN,
                           ProfileConfig::DIP_BIT1_PIN);
  if (rawDipValue_ >= ProfileConfig::PROFILE_COUNT) {
    // Out-of-range DIP value (e.g. 4..7 from a partially seated switch):
    // clamp to profile 0 and report it. Do NOT reboot; the node must stay
    // available for serial provisioning.
    Serial.printf("WARN: DIP value %u out of range; using island_sea\n",
                  static_cast<unsigned>(rawDipValue_));
    rawDipValue_ = 0;
  }
  activeProfile_ = static_cast<ProfileConfig::Profile>(rawDipValue_);

  pinMode(ProfileConfig::BUTTON_PIN, INPUT_PULLUP);
  lastRawButtonLevel_ = digitalRead(ProfileConfig::BUTTON_PIN) == HIGH;
  buttonState_ = ButtonState::Idle;
  buttonStateSinceMs_ = millis();

  buzzerHardwareInit();
  buzzerActive_ = false;
  buzzerOffAtMs_ = 0;

  Serial.printf("PROFILE: %s (dip=%u)\n",
                ProfileConfig::profileName(activeProfile_),
                static_cast<unsigned>(rawDipValue_));
  return true;
}

void ProfileManager::buzzerPulse() {
  buzzerHardwareTone(ProfileConfig::BUZZER_FREQ_HZ);
  buzzerActive_ = true;
  buzzerOffAtMs_ = millis() + ProfileConfig::BUZZER_DURATION_MS;
}

void ProfileManager::task() {
  const uint32_t now = millis();

  // --- Buzzer auto-off -----------------------------------------------------
  if (buzzerActive_ &&
      static_cast<int32_t>(now - buzzerOffAtMs_) >= 0) {
    buzzerHardwareSilence();
    buzzerActive_ = false;
  }

  // --- Button state machine (no delay) ------------------------------------
  const bool rawLevel = digitalRead(ProfileConfig::BUTTON_PIN) == HIGH;

  switch (buttonState_) {
    case ButtonState::Idle:
      if (rawLevel != lastRawButtonLevel_ && rawLevel == false) {
        // Falling edge: button just pressed. Start debounce.
        buttonState_ = ButtonState::Debouncing;
        buttonStateSinceMs_ = now;
      }
      break;

    case ButtonState::Debouncing:
      if (rawLevel == true) {
        // Bounced back to idle before the debounce window closed.
        buttonState_ = ButtonState::Idle;
      } else if (now - buttonStateSinceMs_ >= ProfileConfig::BUTTON_DEBOUNCE_MS) {
        buttonState_ = ButtonState::Holding;
        buttonStateSinceMs_ = now;
      }
      break;

    case ButtonState::Holding:
      if (rawLevel == true) {
        // Released before the long-press threshold: treat as a short press,
        // which is currently a no-op but must NOT fire the long-press callback.
        buttonState_ = ButtonState::Idle;
      } else if (now - buttonStateSinceMs_ >= ProfileConfig::BUTTON_LONG_PRESS_MS) {
        buttonState_ = ButtonState::LongFired;
        if (longPressCallback_) longPressCallback_();
      }
      break;

    case ButtonState::LongFired:
      if (rawLevel == true) {
        // Release after a long press: return to idle so a new press can fire.
        buttonState_ = ButtonState::Idle;
      }
      break;
  }

  lastRawButtonLevel_ = rawLevel;
}
