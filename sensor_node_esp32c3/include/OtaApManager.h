#pragma once

#include <Arduino.h>
#include <cstdint>
#include <mbedtls/md.h>

// FieldRadio ESP32-C3 Sensor Node — OTA over Wi-Fi AP.
//
// COMPILE GUARD: this module is ONLY valid on the ESP32-C3 sensor node.
// The ESP32-S3 LoRa/gateway node has NO OTA by explicit design decision
// (avoid partition bloat). Any attempt to compile this on another target
// must fail at compile time, not silently succeed.
#ifndef SENSOR_NODE_ESP32C3
#error "OtaApManager is ESP32-C3-only; ESP32-S3 gateway has no OTA by design"
#endif

// OtaApManager owns:
//   1. Wi-Fi AP bring-up and shutdown with a bounded provisioning window.
//   2. A WebUI served over the AP for firmware OTA and status.
//   3. ArduinoOTA handler on the same AP.
//   4. NVS-backed state so a reboot preserves the last user intent
//      (AP enabled/disabled), while the runtime AP window remains hard-bounded.
//      without an explicit NVS flag.
//
// SECURITY MODEL (locked by user C3 + mitigations):
//   - The AP is OPEN (no Wi-Fi password) to allow recovery when the stored
//     credential is lost. This is a deliberate trade-off for provisioning.
//   - The AP is time-bounded: the AP automatically shuts down after
//     OTA_AP_WINDOW_MS from AP start. Client activity never extends it.
//   - Firmware upload (both ArduinoOTA and WebUI POST) requires the OTA
//     password already stored in NVS by the existing serial provisioning
//     flow. An open AP does NOT grant upload rights.
//   - The AP can be toggled only by the physical button long-press or by
//     the boot-time NVS flag. There is no remote "enable AP" endpoint.
//   - The WebUI never accepts a new OTA password over the open AP; the OTA
//     password must be set over serial provisioning first.

class OtaApManager {
public:
  static constexpr uint32_t OTA_AP_WINDOW_MS = 10UL * 60UL * 1000UL;  // 10 min
  static constexpr uint32_t PBKDF2_ITERATIONS = 100000;
  static constexpr size_t PASSWORD_SALT_BYTES = 16;
  static constexpr size_t PASSWORD_HASH_BYTES = 32;
  using ProfileChangeCallback = bool (*)(uint8_t profile);
  using RecoveryClearCallback = bool (*)();

  OtaApManager() = default;

  // Reads NVS state and, if the previous run left ap_enabled=true, brings the
  // AP up once. The AP is still bounded by OTA_AP_WINDOW_MS.
  // Returns true if the manager initialized (regardless of AP state).
  bool begin();
  bool beginRecoveryWindow();
  bool provisionPassword(const String& password);
  bool hasOtaCredential() const { return credentialValid_; }
  String otaSigningSaltHex() const;
  uint32_t otaSigningIterations() const { return iterations_; }

  // Non-blocking cooperative task. Handles ArduinoOTA.handle(), WebUI request
  // handling, and the auto-shutdown timer.
  void task();

  // Toggles the AP on or off. Called by ProfileManager's long-press callback.
  // Enabling the AP restarts the provisioning window. Disabling it stops the
  // AP, stops ArduinoOTA, and closes the WebUI server.
  void toggleAp();

  // True while the AP is up (regardless of whether a client is connected).
  bool apEnabled() const { return apEnabled_; }

  // True while a WebUI client is connected. Diagnostic only; the hard
  // provisioning window is never extended by client activity.
  bool clientConnected() const { return clientConnected_; }

  // Registers the runtime profile change handler. The WebUI supplies the
  // profile value and must also confirm that physical sensor cabling was
  // replaced before the callback is invoked.
  void setProfileChangeCallback(ProfileChangeCallback callback) {
    profileChangeCallback_ = callback;
  }
  void setRecoveryClearCallback(RecoveryClearCallback callback) {
    recoveryClearCallback_ = callback;
  }

private:
  bool startAp();
  void stopAp();
  void loadState();
  void saveState();
  void handleRoot();
  void handleUpload();
  void handleUploadDone();
  void handleStatus();
  void handleProfile();
  void handleRecoveryClear();
  bool checkOtaPassword(const String& supplied);
  bool checkSessionToken(const String& supplied) const;
  bool verifyRequestSignature(const String& nonceHex, const String& signatureHex,
                              const uint8_t* body, size_t bodyLen);
  bool beginUploadSignature(const String& nonceHex, const String& signatureHex);
  bool updateUploadSignature(const uint8_t* data, size_t len);
  bool finishUploadSignature();
  bool nonceSeen(const uint8_t nonce[16]) const;
  bool rememberNonce(const uint8_t nonce[16]);
  String sessionToken_;

  static constexpr size_t OTA_NONCE_BYTES = 16;
  static constexpr size_t OTA_NONCE_CACHE_SIZE = 32;
  uint8_t nonceCache_[OTA_NONCE_CACHE_SIZE][OTA_NONCE_BYTES] = {};
  size_t nonceCacheNext_ = 0;
  mbedtls_md_context_t uploadHmac_{};
  uint8_t uploadExpectedSignature_[32] = {};
  uint8_t uploadNonce_[16] = {};
  bool uploadHmacActive_ = false;
  bool uploadAuthorized_ = false;

  bool apEnabled_ = false;
  bool clientConnected_ = false;
  uint32_t apStartedAtMs_ = 0;

  // Loaded by loadState() from the versioned "ota" NVS credential blob.
  // The plaintext OTA password is never persisted.
  uint8_t passwordSalt_[PASSWORD_SALT_BYTES] = {};
  uint8_t passwordHash_[PASSWORD_HASH_BYTES] = {};
  uint32_t iterations_ = PBKDF2_ITERATIONS;
  bool credentialValid_ = false;
  ProfileChangeCallback profileChangeCallback_ = nullptr;
  RecoveryClearCallback recoveryClearCallback_ = nullptr;
};
