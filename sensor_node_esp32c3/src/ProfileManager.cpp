#include "ProfileManager.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>

namespace {

// Non-blocking buzzer helper. Uses the ESP-IDF LEDC peripheral via the
// Arduino-ESP32 wrapper. The exact API depends on the core version:
//   - Arduino-ESP32 3.x: ledcAttach(pin, freq, resolution) at begin() once,
//     then ledcWriteTone(pin, freq) and ledcWrite(pin, 0) to silence.
//   - Arduino-ESP32 2.x: ledcSetup(channel, freq, resolution) +
//     ledcAttachPin(pin, channel) at begin() once, then ledcWriteTone(channel,
//     freq) and ledcWrite(channel, 0) to silence.
//
// The implementation targets Arduino-ESP32 3.x; which core version is in use. The code below uses the
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

bool ProfileManager::loadProfileFromNvs() {
  Preferences prefs;
  if (!prefs.begin("sensor", true)) {
    Serial.println("WARN: sensor NVS open failed; using profile 0");
    activeProfile_ = ProfileConfig::Profile::IslandSea;
    runtimeConfig_ = RuntimeConfig{};
    return false;
  }

  const uint8_t raw = prefs.getUChar("profile", 0);
  ProfileBinding::Tag storedTag{};
  if (prefs.getBytes("profile_bind", storedTag.data(), storedTag.size()) != storedTag.size()) {
    storedTag.fill(0);
  }
  prefs.end();

  if (raw >= ProfileConfig::PROFILE_COUNT) {
    Serial.printf("WARN: stored profile value %u invalid; using profile 0\n",
                  static_cast<unsigned>(raw));
    activeProfile_ = ProfileConfig::Profile::IslandSea;
    runtimeConfig_ = RuntimeConfig{};
    runtimeConfig_.profileBindingTag = ProfileBinding::firmwareProfileHash();
    return false;
  }

  activeProfile_ = static_cast<ProfileConfig::Profile>(raw);
  runtimeConfig_.profile = activeProfile_;
  runtimeConfig_.profileBindingTag = storedTag;
  return true;
}

bool ProfileManager::saveProfileToNvs() {
  Preferences prefs;
  if (!prefs.begin("sensor", false)) {
    Serial.println("ERROR: sensor NVS open failed");
    return false;
  }
  const ProfileBinding::Tag tag = ProfileBinding::firmwareProfileHash();
  const bool profileOk = prefs.putUChar(
      "profile", static_cast<uint8_t>(activeProfile_)) == 1;
  const bool bindOk = prefs.putBytes("profile_bind", tag.data(), tag.size()) == tag.size();
  prefs.end();
  if (!profileOk || !bindOk) {
    Serial.println("ERROR: profile/profile_bind NVS write failed");
    return false;
  }
  runtimeConfig_.profile = activeProfile_;
  runtimeConfig_.profileBindingTag = tag;
  return true;
}

bool ProfileManager::profilePlaceholderDisabled(ProfileConfig::Profile profile) const {
  const uint8_t index = static_cast<uint8_t>(profile);
  if (index >= ProfileConfig::PROFILE_COUNT) return true;
  Preferences prefs;
  if (!prefs.begin("sensor", true)) return true;
  char key[8] = {};
  std::snprintf(key, sizeof(key), "phd%u", static_cast<unsigned>(index));
  const bool disabled = prefs.getBool(key, true);
  prefs.end();
  return disabled;
}

bool ProfileManager::setProfilePlaceholderDisabled(ProfileConfig::Profile profile, bool disabled) {
  const uint8_t index = static_cast<uint8_t>(profile);
  if (index >= ProfileConfig::PROFILE_COUNT) return false;
  Preferences prefs;
  if (!prefs.begin("sensor", false)) return false;
  char key[8] = {};
  std::snprintf(key, sizeof(key), "phd%u", static_cast<unsigned>(index));
  const bool ok = prefs.putBool(key, disabled);
  prefs.end();
  return ok;
}

bool ProfileManager::setProfile(ProfileConfig::Profile profile) {
  if (static_cast<uint8_t>(profile) >= ProfileConfig::PROFILE_COUNT) {
    Serial.println("ERROR: requested profile is out of range");
    return false;
  }
  activeProfile_ = profile;
  return saveProfileToNvs();
}

bool ProfileManager::begin() {
  loadProfileFromNvs();
  const ProfileBinding::Tag expectedTag = ProfileBinding::firmwareProfileHash();
  if (activeProfile_ != ProfileConfig::Profile::IslandSea &&
      !ProfileBinding::equal(runtimeConfig_.profileBindingTag, expectedTag)) {
    Serial.println("AUDIT: PROFILE_BINDING_MISMATCH; forcing profile 0");
    activeProfile_ = ProfileConfig::Profile::IslandSea;
    runtimeConfig_.profile = activeProfile_;
    runtimeConfig_.profileBindingTag = expectedTag;
    Preferences bindPrefs;
    if (bindPrefs.begin("sensor", false)) {
      (void)bindPrefs.putUChar("profile", 0);
      (void)bindPrefs.putBytes("profile_bind", expectedTag.data(), expectedTag.size());
      bindPrefs.end();
    }
  } else if (ProfileBinding::isZero(runtimeConfig_.profileBindingTag)) {
    runtimeConfig_.profileBindingTag = expectedTag;
    Preferences bindPrefs;
    if (bindPrefs.begin("sensor", false)) {
      (void)bindPrefs.putBytes("profile_bind", expectedTag.data(), expectedTag.size());
      bindPrefs.end();
    }
  }
  Preferences prefs;
  if (prefs.begin("sensor", false)) {
    for (uint8_t i = 0; i < ProfileConfig::PROFILE_COUNT; ++i) {
      char key[8] = {};
      std::snprintf(key, sizeof(key), "phd%u", static_cast<unsigned>(i));
      if (!prefs.isKey(key)) (void)prefs.putBool(key, true);
    }
    prefs.end();
  }

  pinMode(ProfileConfig::BUTTON_PIN, INPUT_PULLUP);
  lastRawButtonLevel_ = digitalRead(ProfileConfig::BUTTON_PIN) == HIGH;
  buttonState_ = ButtonState::Idle;
  buttonStateSinceMs_ = millis();

  buzzerHardwareInit();
  buzzerActive_ = false;
  buzzerOffAtMs_ = 0;

  Serial.printf("PROFILE: %s (runtime NVS)\n",
                ProfileConfig::profileName(activeProfile_));
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
