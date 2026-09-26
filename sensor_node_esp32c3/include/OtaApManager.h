#pragma once

#include <Arduino.h>
#include <cstdint>

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
//   2. A WebUI served over the AP for firmware OTA and AP config.
//   3. ArduinoOTA handler on the same AP.
//   4. NVS-backed state so a reboot preserves the last user intent
//      (AP enabled/disabled), but the AP is NEVER auto-enabled on boot
//      without an explicit NVS flag.
//
// SECURITY MODEL (locked by user C3 + mitigations):
//   - The AP is OPEN (no Wi-Fi password) to allow recovery when the stored
//     credential is lost. This is a deliberate trade-off for provisioning.
//   - The AP is time-bounded: the AP automatically shuts down after
//     OTA_AP_WINDOW_MS unless it is explicitly kept alive via WebUI.
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

  OtaApManager() = default;

  // Reads NVS state and, if the previous run left ap_enabled=true, brings the
  // AP up once. Otherwise the AP stays off until toggleAp() is called.
  // Returns true if the manager initialized (regardless of AP state).
  bool begin();

  // Non-blocking cooperative task. Handles ArduinoOTA.handle(), WebUI request
  // handling, and the auto-shutdown timer.
  void task();

  // Toggles the AP on or off. Called by ProfileManager's long-press callback.
  // Enabling the AP restarts the provisioning window. Disabling it stops the
  // AP, stops ArduinoOTA, and closes the WebUI server.
  void toggleAp();

  // True while the AP is up (regardless of whether a client is connected).
  bool apEnabled() const { return apEnabled_; }

  // True while a WebUI client is connected. Used to keep the AP alive past
  // the auto-shutdown window.
  bool clientConnected() const { return clientConnected_; }

private:
  bool startAp();
  void stopAp();
  void loadState();
  void saveState();
  void handleRoot();
  void handleUpload();
  void handleUploadDone();
  void handleConfig();
  void handleStatus();
  bool checkOtaPassword(const String& supplied);

  bool apEnabled_ = false;
  bool clientConnected_ = false;
  uint32_t apStartedAtMs_ = 0;
  uint32_t lastClientSeenMs_ = 0;

  // Loaded by loadState() from the "ota" NVS namespace.
  // Green-field OTA has no station-mode credential path.
  String otaPassword_;
  String lastClientIp_;
};
