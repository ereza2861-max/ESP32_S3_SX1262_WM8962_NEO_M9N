Iterasi C — Pesan 6d dari 7: handleConfig() — Handler Terakhir

Mengapa Handler Ini Terpisah

handleConfig adalah handler paling kompleks di seluruh WebUI (~250 baris):

1. Membaca ~50 argumen dari body application/x-www-form-urlencoded (via req.arg(...)).
2. Memvalidasi setiap field terhadap bound di Config.h.
3. Menyimpan snapshot previous untuk rollback.
4. Menggunakan configApplyTransaction(candidate, generation, apply, rollback) untuk apply atomic.
5. Meng-update gState.rangeTest, gState.hopChannelList setelah commit.

Kalau saya selipkan di Pesan 6c, pesan akan terpotong. Jadi ini pesan terpisah.

Salin dan Sambung ke WebUi.cpp

Hapus dulu baris // === END OF WebUi.cpp ===, lalu salin blok di bawah ini. Setelah ini, WebUi.cpp benar-benar selesai.

---

```cpp
// ---------------------------------------------------------------------------
// Handlers: config (POST /api/config) — the last remaining handler
// ---------------------------------------------------------------------------
void WebUi::handleConfig(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig candidate{};
  uint32_t configGenerationSnapshot = 0;
  if (!configSnapshot(candidate, configGenerationSnapshot)) {
    res.send503("configuration snapshot unavailable");
    return;
  }
  if (!rateLimit(req, res, lastConfigMs_, candidate.webAuthRateLimitMs)) return;
  bool radioChanged = false;

  auto parseUnsigned = [](const String& raw, uint32_t maxValue, uint32_t& out) {
    return WebUiNumericParser::parseUnsigned(raw.c_str(), raw.length(), maxValue, out);
  };

  auto validHex32 = [](const String& raw) {
    if (raw.length() != 32) return false;
    for (size_t i = 0; i < raw.length(); ++i) {
      const char c = raw[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')))
        return false;
    }
    return true;
  };
  auto validPassword = [](const String& raw) {
    if (raw.length() < 8 || raw.length() > 63) return false;
    for (size_t i = 0; i < raw.length(); ++i) {
      const uint8_t c = static_cast<uint8_t>(raw[i]);
      if (c < 0x20 || c == 0x7F || c == '\\' || c == '"') return false;
    }
    return true;
  };
  auto parseBoolArg = [&](const char* name, bool& out) {
    if (!req.hasArg(name)) return true;
    const String raw = req.arg(name);
    if (raw == "0") { out = false; return true; }
    if (raw == "1") { out = true;  return true; }
    return false;
  };

  // ---- Radio ----
  if (req.hasArg("freq")) {
    const String raw = req.arg("freq");
    char* end = nullptr;
    const float value = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(value)) {
      res.sendText(400, "invalid frequency"); return;
    }
    candidate.loraFreqMHz = value;
    radioChanged = true;
  }
  if (req.hasArg("bw")) {
    const String raw = req.arg("bw");
    char* end = nullptr;
    const float value = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(value)) {
      res.sendText(400, "invalid bandwidth"); return;
    }
    candidate.loraBwKHz = value;
    radioChanged = true;
  }

  uint32_t value = 0;
  if (req.hasArg("sf")) {
    if (!parseUnsigned(req.arg("sf"), 12, value) || value < 5) {
      res.sendText(400, "invalid spreading factor"); return;
    }
    candidate.loraSf = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (req.hasArg("cr")) {
    if (!parseUnsigned(req.arg("cr"), 8, value) || value < 5) {
      res.sendText(400, "invalid coding rate"); return;
    }
    candidate.loraCr = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (req.hasArg("sync")) {
    if (!parseUnsigned(req.arg("sync"), 255, value)) {
      res.sendText(400, "invalid sync word"); return;
    }
    candidate.loraSyncWord = static_cast<uint8_t>(value);
    radioChanged = true;
  }
  if (req.hasArg("power")) {
    if (!parseUnsigned(req.arg("power"), 17, value) || value < 2) {
      res.sendText(400, "invalid power"); return;
    }
    candidate.loraPowerDbm = static_cast<int8_t>(value);
    radioChanged = true;
  }

  // ---- Audio ----
  if (req.hasArg("volume")) {
    if (!parseUnsigned(req.arg("volume"), 100, value)) {
      res.sendText(400, "invalid volume"); return;
    }
    candidate.volume = static_cast<uint8_t>(value);
  }
  if (req.hasArg("audio_source")) {
    if (!parseUnsigned(req.arg("audio_source"), Config::AUDIO_SOURCE_USB, value)) {
      res.sendText(400, "invalid audio source"); return;
    }
    candidate.audioRecordSource = static_cast<uint8_t>(value);
  }
  if (req.hasArg("batcal")) {
    const String raw = req.arg("batcal");
    char* end = nullptr;
    const float batcal = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(batcal) ||
        batcal < 0.5f || batcal > 1.5f) {
      res.sendText(400, "invalid battery calibration"); return;
    }
    candidate.batteryCalibration = batcal;
  }

  // ---- Identity / credentials ----
  if (req.hasArg("callsign")) candidate.callsign = req.arg("callsign");
  if (req.hasArg("lora_key")) candidate.loraKeyHex = req.arg("lora_key");
  if (req.hasArg("ap_password")) candidate.apPassword = req.arg("ap_password");
  if (req.hasArg("sta_ssid")) {
    candidate.staSsid = req.arg("sta_ssid");
    if (candidate.staSsid.length() > Config::STA_SSID_MAX_LEN) {
      res.sendText(400, "invalid STA SSID"); return;
    }
    if (candidate.staSsid.isEmpty()) candidate.staPassword.clear();
  }
  if (req.hasArg("sta_password")) {
    candidate.staPassword = req.arg("sta_password");
    if (!candidate.staSsid.isEmpty() &&
        (candidate.staPassword.length() < Config::STA_PASSWORD_MIN_LEN ||
         candidate.staPassword.length() > Config::STA_PASSWORD_MAX_LEN ||
         !validPassword(candidate.staPassword))) {
      res.sendText(400, "invalid STA password"); return;
    }
  }
  if (req.hasArg("web_password")) candidate.webPassword = req.arg("web_password");

  // ---- BLE pairing ----
  if (req.hasArg("ble_pairing")) {
    const String raw = req.arg("ble_pairing");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid BLE pairing setting"); return;
    }
    candidate.blePairingEnabled = raw == "1";
  }

  // ---- MQTT ----
  if (req.hasArg("mqtt_enabled")) {
    const String raw = req.arg("mqtt_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid MQTT setting"); return;
    }
    candidate.mqttEnabled = raw == "1";
  }
  if (req.hasArg("mqtt_host")) {
    candidate.mqttHost = req.arg("mqtt_host");
    if (candidate.mqttHost.isEmpty() || candidate.mqttHost.length() > 253 ||
        candidate.mqttHost.indexOf('|') >= 0) {
      res.sendText(400, "invalid MQTT host"); return;
    }
  }
  if (req.hasArg("mqtt_port")) {
    if (!parseUnsigned(req.arg("mqtt_port"), 65535, value) || value == 0) {
      res.sendText(400, "invalid MQTT port"); return;
    }
    candidate.mqttPort = static_cast<uint16_t>(value);
  }
  if (req.hasArg("mqtt_tls")) {
    const String raw = req.arg("mqtt_tls");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid MQTT TLS setting"); return;
    }
    if (Config::mqttTlsIsMandatory() && raw == "0") {
      res.sendText(400, "MQTT TLS is mandatory in this build"); return;
    }
    candidate.mqttTlsRequired = raw == "1";
  }
  if (req.hasArg("mqtt_reconnect_min_ms")) {
    if (!parseUnsigned(req.arg("mqtt_reconnect_min_ms"),
                       Config::MQTT_RECONNECT_MS_MAX, value) ||
        value < Config::MQTT_RECONNECT_MS_MIN) {
      res.sendText(400, "invalid MQTT reconnect minimum"); return;
    }
    candidate.mqttReconnectMinMs = value;
  }
  if (req.hasArg("mqtt_reconnect_max_ms")) {
    if (!parseUnsigned(req.arg("mqtt_reconnect_max_ms"),
                       Config::MQTT_RECONNECT_MS_MAX, value) ||
        value < Config::MQTT_RECONNECT_MS_MIN) {
      res.sendText(400, "invalid MQTT reconnect maximum"); return;
    }
    candidate.mqttReconnectMaxMs = value;
  }
  if (req.hasArg("mqtt_telemetry_period_ms")) {
    if (!parseUnsigned(req.arg("mqtt_telemetry_period_ms"),
                       Config::MQTT_TELEMETRY_PERIOD_MS_MAX, value) ||
        value < Config::MQTT_TELEMETRY_PERIOD_MS_MIN) {
      res.sendText(400, "invalid MQTT telemetry period"); return;
    }
    candidate.mqttTelemetryPeriodMs = value;
  }
  if (req.hasArg("mqtt_health_period_ms")) {
    if (!parseUnsigned(req.arg("mqtt_health_period_ms"), 86400000, value) ||
        value < 1000) {
      res.sendText(400, "invalid MQTT health period"); return;
    }
    candidate.mqttHealthPeriodMs = value;
  }
  if (!parseBoolArg("mqtt_retain_telemetry", candidate.mqttRetainTelemetry) ||
      !parseBoolArg("mqtt_retain_availability", candidate.mqttRetainAvailability)) {
    res.sendText(400, "invalid MQTT retain setting"); return;
  }
  if (req.hasArg("mqtt_rotation_days")) {
    if (!parseUnsigned(req.arg("mqtt_rotation_days"), 3650, value) || value < 1) {
      res.sendText(400, "invalid MQTT rotation policy"); return;
    }
    candidate.mqttCredentialRotationDays = static_cast<uint16_t>(value);
  }

  // ---- EST / certificate lifecycle ----
  if (req.hasArg("est_server_url")) {
    candidate.estServerUrl = req.arg("est_server_url");
    if (candidate.estServerUrl.length() > 253 ||
        (!candidate.estServerUrl.isEmpty() &&
         !candidate.estServerUrl.startsWith("https://"))) {
      res.sendText(400, "invalid EST server URL"); return;
    }
  }
  if (req.hasArg("est_label")) {
    candidate.estLabel = req.arg("est_label");
    if (candidate.estLabel.isEmpty() || candidate.estLabel.length() > 95 ||
        !candidate.estLabel.startsWith("/") ||
        candidate.estLabel.indexOf('|') >= 0) {
      res.sendText(400, "invalid EST label"); return;
    }
  }
  if (req.hasArg("cert_renewal_threshold_days")) {
    if (!parseUnsigned(req.arg("cert_renewal_threshold_days"),
                       Config::CERT_RENEWAL_THRESHOLD_DAYS_MAX, value) ||
        value < Config::CERT_RENEWAL_THRESHOLD_DAYS_MIN) {
      res.sendText(400, "invalid certificate renewal threshold"); return;
    }
    candidate.certRenewalThresholdDays = static_cast<uint16_t>(value);
  }
  if (req.hasArg("cert_check_period_ms")) {
    if (!parseUnsigned(req.arg("cert_check_period_ms"), 7UL * 86400000UL, value) ||
        value < 3600000UL) {
      res.sendText(400, "invalid certificate check period"); return;
    }
    candidate.certCheckPeriodMs = value;
  }
  if (req.hasArg("est_auth_mode")) {
    if (!parseUnsigned(req.arg("est_auth_mode"), 2, value)) {
      res.sendText(400, "invalid EST auth mode"); return;
    }
    candidate.estAuthMode = static_cast<uint8_t>(value);
  }
  if (req.hasArg("est_username")) {
    candidate.estUsername = req.arg("est_username");
    if (candidate.estUsername.length() > 64) {
      res.sendText(400, "invalid EST username"); return;
    }
    for (size_t i = 0; i < candidate.estUsername.length(); ++i)
      if (static_cast<uint8_t>(candidate.estUsername[i]) < 0x20 ||
          static_cast<uint8_t>(candidate.estUsername[i]) == 0x7F) {
        res.sendText(400, "invalid EST username"); return;
      }
  }
  if (req.hasArg("est_password")) {
    candidate.estPassword = req.arg("est_password");
    if (candidate.estPassword.length() > 64) {
      res.sendText(400, "invalid EST password"); return;
    }
    for (size_t i = 0; i < candidate.estPassword.length(); ++i)
      if (static_cast<uint8_t>(candidate.estPassword[i]) < 0x20 ||
          static_cast<uint8_t>(candidate.estPassword[i]) == 0x7F) {
        res.sendText(400, "invalid EST password"); return;
      }
  }
  if (req.hasArg("est_bootstrap_token")) {
    candidate.estBootstrapToken = req.arg("est_bootstrap_token");
    if (candidate.estBootstrapToken.length() > 128) {
      res.sendText(400, "invalid EST bootstrap token"); return;
    }
    for (size_t i = 0; i < candidate.estBootstrapToken.length(); ++i)
      if (static_cast<uint8_t>(candidate.estBootstrapToken[i]) < 0x20 ||
          static_cast<uint8_t>(candidate.estBootstrapToken[i]) == 0x7F) {
        res.sendText(400, "invalid EST bootstrap token"); return;
      }
  }
  if (req.hasArg("cert_lifecycle_enabled")) {
    const String raw = req.arg("cert_lifecycle_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid certificate lifecycle setting"); return;
    }
    candidate.certLifecycleEnabled = raw == "1";
  }

  // ---- Deep sleep / wake ----
  if (req.hasArg("wake_period_sec")) {
    if (!parseUnsigned(req.arg("wake_period_sec"),
                       Config::WAKE_PERIOD_SEC_MAX, value) ||
        value < Config::WAKE_PERIOD_SEC_MIN) {
      res.sendText(400, "invalid wake period"); return;
    }
    candidate.wakePeriodSec = value;
  }
  if (req.hasArg("deep_sleep_enabled")) {
    const String raw = req.arg("deep_sleep_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid deep-sleep setting"); return;
    }
    candidate.deepSleepEnabled = raw == "1";
  }
  if (req.hasArg("deep_sleep_idle_sec")) {
    if (!parseUnsigned(req.arg("deep_sleep_idle_sec"),
                       Config::DEEP_SLEEP_IDLE_MS_MAX / 1000UL, value) ||
        value < Config::DEEP_SLEEP_IDLE_MS_MIN / 1000UL) {
      res.sendText(400, "invalid deep-sleep idle timeout"); return;
    }
    candidate.deepSleepIdleMs = value * 1000UL;
  }
  if (req.hasArg("deep_sleep_wake_grace_ms")) {
    if (!parseUnsigned(req.arg("deep_sleep_wake_grace_ms"), 60000, value) ||
        value < 100) {
      res.sendText(400, "invalid wake grace period"); return;
    }
    candidate.deepSleepWakeGraceMs = value;
  }
  if (req.hasArg("critical_shutdown_delay_ms")) {
    if (!parseUnsigned(req.arg("critical_shutdown_delay_ms"), 600000, value) ||
        value < 100) {
      res.sendText(400, "invalid critical shutdown delay"); return;
    }
    candidate.criticalShutdownDelayMs = value;
  }
  if (req.hasArg("battery_low_threshold") ||
      req.hasArg("battery_critical_threshold")) {
    const String lowRaw = req.hasArg("battery_low_threshold")
        ? req.arg("battery_low_threshold")
        : String(candidate.batteryLowThreshold, 3);
    const String criticalRaw = req.hasArg("battery_critical_threshold")
        ? req.arg("battery_critical_threshold")
        : String(candidate.batteryCriticalThreshold, 3);
    char* lowEnd = nullptr;
    char* criticalEnd = nullptr;
    const float low = strtof(lowRaw.c_str(), &lowEnd);
    const float critical = strtof(criticalRaw.c_str(), &criticalEnd);
    if (!lowEnd || *lowEnd != '\0' || !criticalEnd || *criticalEnd != '\0' ||
        !isfinite(low) || !isfinite(critical) || critical < 2.5f ||
        low <= critical || low > 4.2f) {
      res.sendText(400, "invalid battery thresholds"); return;
    }
    candidate.batteryLowThreshold = low;
    candidate.batteryCriticalThreshold = critical;
  }

  // ---- Class-D ----
  if (req.hasArg("classd_enabled")) {
    const String raw = req.arg("classd_enabled");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid Class-D setting"); return;
    }
    candidate.classDEnabled = raw == "1";
  }
  if (req.hasArg("classd_boost")) {
    if (!parseUnsigned(req.arg("classd_boost"), 7, value)) {
      res.sendText(400, "invalid Class-D boost"); return;
    }
    candidate.classDBoostLevel = static_cast<uint8_t>(value);
  }

  // ---- VOX ----
  if (!parseBoolArg("vox_enabled", candidate.voxEnabled)) {
    res.sendText(400, "invalid VOX setting"); return;
  }
  if (req.hasArg("vox_threshold")) {
    const String raw = req.arg("vox_threshold");
    char* end = nullptr;
    const float threshold = strtof(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(threshold)) {
      res.sendText(400, "invalid VOX threshold"); return;
    }
    candidate.voxThreshold = threshold;
  }
  if (req.hasArg("vox_hang_ms")) {
    if (!parseUnsigned(req.arg("vox_hang_ms"), 10000, value) || value < 50) {
      res.sendText(400, "invalid VOX hang time"); return;
    }
    candidate.voxHangMs = value;
  }

  // ---- Booleans ----
  if (!parseBoolArg("aec_enabled", candidate.aecEnabled) ||
      !parseBoolArg("usb_monitor", candidate.usbMonitor) ||
      !parseBoolArg("usb_transport", candidate.usbPlaybackTransport) ||
      !parseBoolArg("loopback", candidate.audioLoopback) ||
      !parseBoolArg("adr_enabled", candidate.loraAdrEnabled) ||
      !parseBoolArg("hop_enabled", candidate.loraHopEnabled)) {
    res.sendText(400, "invalid boolean configuration"); return;
  }
  if (req.hasArg("hop_profile")) {
    if (!parseUnsigned(req.arg("hop_profile"), Config::HOP_CHANNEL_MAX, value) ||
        value == 0) {
      res.sendText(400, "invalid hop profile"); return;
    }
    candidate.loraHopChannelProfile = static_cast<uint8_t>(value);
  }
  if (req.hasArg("range_test_mode")) {
    const String raw = req.arg("range_test_mode");
    if (raw != "0" && raw != "1") {
      res.sendText(400, "invalid range-test mode"); return;
    }
    candidate.loraRangeTestMode = raw == "1";
  }

  // ---- BLE reader ----
  if (!parseBoolArg("ble_enabled", candidate.sensorReaderEnabled)) {
    res.sendText(400, "invalid BLE reader setting"); return;
  }
  if (req.hasArg("ble_scan_interval_ms")) {
    if (!parseUnsigned(req.arg("ble_scan_interval_ms"),
                       Config::BLE_SCAN_INTERVAL_MS_MAX, value) ||
        value < Config::BLE_SCAN_INTERVAL_MS_MIN) {
      res.sendText(400, "invalid BLE scan interval"); return;
    }
    candidate.sensorScanIntervalMs = value;
  }
  if (req.hasArg("ble_scan_window_ms")) {
    if (!parseUnsigned(req.arg("ble_scan_window_ms"), 60000, value) ||
        value == 0) {
      res.sendText(400, "invalid BLE scan window"); return;
    }
    candidate.sensorScanWindowMs = static_cast<uint16_t>(value);
  }
  if (req.hasArg("ble_scan_duration_ms")) {
    if (!parseUnsigned(req.arg("ble_scan_duration_ms"), 60000, value) ||
        value < 100) {
      res.sendText(400, "invalid BLE scan duration"); return;
    }
    candidate.sensorScanDurationMs = value;
  }
  if (req.hasArg("ble_connect_timeout_ms")) {
    if (!parseUnsigned(req.arg("ble_connect_timeout_ms"), 30000, value) ||
        value < 500) {
      res.sendText(400, "invalid BLE connect timeout"); return;
    }
    candidate.sensorConnectTimeoutMs = value;
  }
  if (req.hasArg("ble_eviction_ms")) {
    if (!parseUnsigned(req.arg("ble_eviction_ms"), 7UL * 86400000UL, value) ||
        value < 10000) {
      res.sendText(400, "invalid BLE eviction timeout"); return;
    }
    candidate.sensorNodeEvictionMs = value;
  }
  if (req.hasArg("ble_max_nodes")) {
    if (!parseUnsigned(req.arg("ble_max_nodes"),
                       Config::SENSOR_MAX_NODES_VALUE, value) || value == 0) {
      res.sendText(400, "invalid BLE maximum nodes"); return;
    }
    candidate.sensorMaxNodes = static_cast<uint8_t>(value);
  }
  if (!parseBoolArg("ble_encryption", candidate.sensorRequireEncryption)) {
    res.sendText(400, "invalid BLE encryption setting"); return;
  }
  if (req.hasArg("ble_failure_threshold")) {
    if (!parseUnsigned(req.arg("ble_failure_threshold"), 20, value) || value == 0) {
      res.sendText(400, "invalid BLE pairing failure threshold"); return;
    }
    candidate.blePairingFailureThreshold = static_cast<uint8_t>(value);
  }
  if (req.hasArg("ble_block_ms")) {
    if (!parseUnsigned(req.arg("ble_block_ms"), 86400000UL, value) ||
        value < 1000) {
      res.sendText(400, "invalid BLE pairing block time"); return;
    }
    candidate.blePairingBlockMs = value;
  }
  if (!parseBoolArg("ble_keep_awake", candidate.sensorKeepAwake)) {
    res.sendText(400, "invalid BLE keep-awake setting"); return;
  }

  // ---- Web security policy ----
  if (req.hasArg("web_session_timeout_ms")) {
    if (!parseUnsigned(req.arg("web_session_timeout_ms"),
                       Config::WEB_SESSION_TIMEOUT_MS_MAX, value) ||
        value < Config::WEB_SESSION_TIMEOUT_MS_MIN) {
      res.sendText(400, "invalid web session timeout"); return;
    }
    candidate.webSessionTimeoutMs = value;
  }
  if (req.hasArg("web_auth_rate_limit_ms")) {
    if (!parseUnsigned(req.arg("web_auth_rate_limit_ms"),
                       Config::WEB_AUTH_RATE_LIMIT_MS_MAX, value) ||
        value < Config::WEB_AUTH_RATE_LIMIT_MS_MIN) {
      res.sendText(400, "invalid auth rate limit"); return;
    }
    candidate.webAuthRateLimitMs = value;
  }
  if (req.hasArg("csrf_policy")) {
    if (!parseUnsigned(req.arg("csrf_policy"), 1, value)) {
      res.sendText(400, "invalid CSRF policy"); return;
    }
    candidate.csrfPolicy = static_cast<uint8_t>(value);
  }
  if (req.hasArg("ble_pairing_policy")) {
    if (!parseUnsigned(req.arg("ble_pairing_policy"), 1, value)) {
      res.sendText(400, "invalid BLE pairing policy"); return;
    }
    candidate.blePairingPolicy = static_cast<uint8_t>(value);
  }
  if (req.hasArg("ecdh_rekey_policy")) {
    if (!parseUnsigned(req.arg("ecdh_rekey_policy"), 1, value)) {
      res.sendText(400, "invalid ECDH rekey policy"); return;
    }
    candidate.ecdhRekeyPolicy = static_cast<uint8_t>(value);
  }
  if (req.hasArg("replay_window_bits")) {
    if (!parseUnsigned(req.arg("replay_window_bits"),
                       Config::LORA_REPLAY_WINDOW_BITS, value) ||
        value < Config::LORA_REPLAY_WINDOW_BITS_MIN) {
      res.sendText(400, "replay window must be 8..32 bits"); return;
    }
    candidate.replayWindowBits = static_cast<uint8_t>(value);
  }

  // ---- Cross-field validation ----
  if (!candidate.validSemantics() || candidate.volume > 100 ||
      candidate.audioRecordSource > Config::AUDIO_SOURCE_USB ||
      candidate.wakePeriodSec < Config::WAKE_PERIOD_SEC_MIN ||
      candidate.wakePeriodSec > Config::WAKE_PERIOD_SEC_MAX ||
      candidate.deepSleepIdleMs < 60000UL ||
      candidate.deepSleepIdleMs > 86400000UL ||
      candidate.deepSleepWakeGraceMs < 100UL ||
      candidate.deepSleepWakeGraceMs > 60000UL ||
      candidate.criticalShutdownDelayMs < 100UL ||
      candidate.criticalShutdownDelayMs > 600000UL ||
      !isfinite(candidate.batteryLowThreshold) ||
      !isfinite(candidate.batteryCriticalThreshold) ||
      candidate.batteryCriticalThreshold < Config::BATTERY_CRITICAL_THRESHOLD_MIN ||
      candidate.batteryLowThreshold <= candidate.batteryCriticalThreshold ||
      candidate.batteryLowThreshold > Config::BATTERY_LOW_THRESHOLD_MAX ||
      candidate.classDBoostLevel > 7 ||
      candidate.staSsid.length() > Config::STA_SSID_MAX_LEN ||
      ((!candidate.staSsid.isEmpty()) &&
       (candidate.staPassword.length() < Config::STA_PASSWORD_MIN_LEN ||
        candidate.staPassword.length() > Config::STA_PASSWORD_MAX_LEN)) ||
      (candidate.classDEnabled && !Config::CLASS_D_ENABLED) ||
      !validHex32(candidate.loraKeyHex) ||
      !validPassword(candidate.apPassword) ||
      (!candidate.webPassword.isEmpty() && !validPassword(candidate.webPassword)) ||
      candidate.apSsid.isEmpty() || candidate.webUser.isEmpty() ||
      !candidate.webPasswordConfigured() ||
      (!candidate.webPassword.isEmpty() &&
       candidate.apPassword == candidate.webPassword)) {
    res.sendText(400, "invalid configuration");
    return;
  }

  RuntimeConfig previous{};
  if (!configSnapshot(previous)) {
    res.send503("configuration snapshot unavailable");
    return;
  }
  const uint8_t previousSource = audio.recordSource();

  const bool committed = configApplyTransaction(
      candidate, configGenerationSnapshot,
      [&]() {
        if (candidate.audioRecordSource != previousSource &&
            !audio.setRecordSource(candidate.audioRecordSource)) return false;
        if (!audio.setClassDConfig(candidate.classDEnabled,candidate.classDBoostLevel) ||
            !audio.setVox(candidate.voxEnabled,candidate.voxThreshold,candidate.voxHangMs) ||
            !audio.setAec(candidate.aecEnabled) ||
            !audio.setUsbMonitor(candidate.usbMonitor) ||
            !audio.setUsbPlaybackTransport(candidate.usbPlaybackTransport) ||
            !audio.setLoopback(candidate.audioLoopback)) return false;
        if (!mqtt.applyConfig(candidate)) return false;
        if (!lora.setAdrEnabled(candidate.loraAdrEnabled)) return false;
        if (radioChanged && !lora.applyConfig(candidate)) return false;
        audio.setVolume(candidate.volume);
        return true;
      },
      [&]() {
        bool ok = true;
        ok = audio.setClassDConfig(previous.classDEnabled, previous.classDBoostLevel) && ok;
        ok = audio.setVox(previous.voxEnabled, previous.voxThreshold, previous.voxHangMs) && ok;
        ok = audio.setAec(previous.aecEnabled) && ok;
        ok = audio.setUsbMonitor(previous.usbMonitor) && ok;
        ok = audio.setUsbPlaybackTransport(previous.usbPlaybackTransport) && ok;
        ok = audio.setLoopback(previous.audioLoopback) && ok;
        ok = audio.setRecordSource(previousSource) && ok;
        ok = lora.setAdrEnabled(previous.loraAdrEnabled) && ok;
        ok = mqtt.applyConfig(previous) && ok;
        if (radioChanged) ok = lora.applyConfig(previous) && ok;
        audio.setVolume(previous.volume);
        return ok;
      });

  if (!committed) {
    res.send503("configuration transaction failed; runtime and persistence rolled back");
    return;
  }
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.rangeTest = candidate.loraRangeTestMode;
      const uint8_t profileCount = min<uint8_t>(
          max<uint8_t>(1U, candidate.loraHopChannelProfile),
          Config::HOP_CHANNEL_MAX);
      gState.hopChannelCount = profileCount;
      for (uint8_t i = 0; i < Config::HOP_CHANNEL_MAX; ++i)
        gState.hopChannelList[i] = i < profileCount ? i : 0;
    }
  }
  lora.updateSourceId();
  auditConfigChange(previous, gConfig, "web");
  res.sendText(200,
               "Configuration saved; BLE pairing and WiFi credential changes apply after reboot");
}

// === END OF WebUi.cpp ===
```

---

Status

WebUi.cpp selesai total sekarang. 121 handler + helper + begin() + task().

File Status
HttpdServer.h/.cpp ✅
HttpdRequest.h/.cpp ✅
HttpdResponse.h/.cpp ✅
HttpdMultipart.h/.cpp ✅
WebUi.h ✅
WebUi.cpp ✅ SELESAI
main.cpp diff ⏳ Pesan 7
platformio.ini diff ⏳ Pesan 7

Cara Verifikasi Sementara Sebelum Pesan 7

Setelah semua file tersalin, kalau Anda coba build sekarang:

1. Error yang diharapkan:
   · main.cpp masih include ESPWebServerSecure.hpp — file itu masih ada di .pio/libdeps/, jadi link error.
   · main.cpp deklarasi ESPWebServerSecure server(Config::WEB_PORT); WebUi web(server); — tipe mismatch, WebUi sekarang butuh HttpdServer&.
2. Error yang mungkin muncul dan perlu saya perbaiki:
   · HttpdServer.h include <esp_https_server.h> — butuh CONFIG_ESP_HTTPS_SERVER_ENABLE=y di sdkconfig. Kalau error #include <esp_https_server.h> not found, kita tambahkan sdkconfig.defaults.
   · httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT(); — designated initializer C di C++.
   · std::nothrow butuh <new> di HttpdServer.cpp dan HttpdRequest.cpp.

Jangan build dulu. Tunggu Pesan 7.

Konfirmasi Pesan 6d tersalin, saya lanjut ke Pesan 7: main.cpp diff + platformio.ini diff.