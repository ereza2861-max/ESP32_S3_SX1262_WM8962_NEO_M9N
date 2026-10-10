Iterasi C — Pesan 6 dari 7: WebUi.cpp Bagian 3

Peringatan Penting: Handler Masih Sangat Panjang

Bagian ini berisi ~50 handler dan tetap sangat panjang. Saya masih harus memecahnya lagi menjadi 3 sub-pesan agar tidak terpotong di chat:

· Pesan 6a (ini): handler root/status/version/LoRaWAN/files/upload/rename/messages/schedule/SOS/selftest/UI prefs (~30 handler)
· Pesan 6b: handler diagnostics/capture/ADR/HOP/scanner/range-test/radio/battery/storage/audio (~50 handler)
· Pesan 6c: handler track/sensor/BLE/MQTT/config/factory-reset/reboot (~40 handler)

Anda sambung ketiganya menjadi Bagian 3 utuh di WebUi.cpp, setelah // === SAMBUNG KE BAGIAN 3 (Pesan 6) ===.

Salin dan Sambung ke WebUi.cpp

Hapus dulu baris // === SAMBUNG KE BAGIAN 3 (Pesan 6) ===, lalu salin ini:

---

```cpp
// ---------------------------------------------------------------------------
// Handlers: root & meta
// ---------------------------------------------------------------------------
void WebUi::handleRoot(HttpdRequest& req, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.setHeader("X-Content-Type-Options", "nosniff");
  res.setHeader("X-Frame-Options", "DENY");
  res.setHeader("Referrer-Policy", "no-referrer");
  res.setHeader("Content-Security-Policy",
                "default-src 'self'; script-src 'self' 'unsafe-inline' https://unpkg.com; "
                "style-src 'self' 'unsafe-inline' https://unpkg.com; object-src 'none'; "
                "img-src 'self' data: https://*.tile.openstreetmap.org; "
                "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'");
  String page = FPSTR(INDEX_HTML);
  page.replace("__CSRF_TOKEN__", csrfTokenHexForActiveSession());
  Preferences themePrefs;
  String persistedTheme = "dark";
  if (themePrefs.begin("fieldradio", true)) { persistedTheme = themePrefs.getString("theme", "dark"); themePrefs.end(); }
  page.replace("__THEME_CLASS__", persistedTheme == "light" ? "light" : "");
  page.replace("__MQTT_TLS_DISABLED__", Config::mqttTlsIsMandatory() ? " disabled" : "");
  res.sendHtml(200, page);
}

void WebUi::handleApiVersion(HttpdRequest& req, HttpdResponse& res) {
  (void)req;
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, "{\"api\":1,\"protocol\":" +
                     String(Config::LORA_PROTOCOL_VERSION) +
                     ",\"secureBootV2\":" +
                     String(CONFIG_SECURE_BOOT_V2_ENABLED ? "true" : "false") +
                     ",\"flashEncryption\":" +
                     String(CONFIG_SECURE_FLASH_ENC_ENABLED ? "true" : "false") +
                     "}");
}

void WebUi::handleStatus(HttpdRequest& req, HttpdResponse& res) {
  (void)req;
  RuntimeConfig config;
  if (!configSnapshot(config)) {
    res.send503("configuration unavailable");
    return;
  }
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }

  String j = "{";
  j += "\"gps\":{\"valid\":" + String(gState.gps.valid ? "true":"false");
  j += ",\"lat\":" + String(gState.gps.lat,6);
  j += ",\"lon\":" + String(gState.gps.lon,6);
  j += ",\"alt\":" + String(gState.gps.alt,1);
  j += ",\"sat\":" + String(gState.gps.satellites);
  j += ",\"timeValid\":" + String(gState.gps.timeValid ? "true":"false");
  j += ",\"utcEpoch\":" + String(static_cast<unsigned long long>(gState.gps.utcEpoch)) + "},";
  j += "\"lora\":{\"ready\":" + String(gState.loraReady ? "true":"false");
  j += ",\"rssi\":" + String(gState.loraRssi) + ",\"snr\":" + String(gState.loraSnr,1);
  j += ",\"lqi\":" + String(lora.lqi());
  j += ",\"sf\":" + String(lora.currentDataRate());
  j += ",\"adr\":" + String(lora.adrEnabled() ? "true" : "false") + "},";
  j += "\"codec\":" + String(gState.codecReady ? "true":"false") + ",";
  j += "\"sensorDropped\":" + String(gState.sensorDropped) + ",";
  j += "\"sensorSpoolDepth\":" + String(gState.sensorSpoolDepth) + ",";
  j += "\"sensorSpoolEvictions\":" + String(gState.sensorSpoolEvictions) + ",";
  j += "\"sensorSpoolDrops\":" + String(gState.sensorSpoolDrops) + ",";
  j += "\"sensorSpoolRecovered\":" + String(gState.sensorSpoolRecovered) + ",";
  j += "\"peerMacFailures\":" + String(gState.peerMacFailures) + ",";
  j += "\"sd\":" + String(gState.storageReady ? "true":"false") + ",";
  j += "\"battery\":{\"available\":" + String(gState.batteryAvailable ? "true":"false");
  j += ",\"v\":";
  j += gState.batteryAvailable ? String(gState.batteryV, 2) : "null";
  j += ",\"low\":" + String(gState.batteryLow ? "true":"false");
  j += ",\"percent\":" + String(gState.batteryPercent);
  j += ",\"estimatedMinutes\":" + String(gState.batteryEstimatedMinutes);
  j += ",\"critical\":" + String(gState.batteryCritical ? "true":"false") + "},";
  j += "\"usbAudio\":" + String(gState.usbAudioReady ? "true":"false") + ",";
  j += "\"usbAudioActive\":" + String(gState.usbAudioActive ? "true":"false") + ",";
  j += "\"audioSource\":" + String(audio.recordSource()) + ",";
  j += "\"usbMuted\":" + String(gState.usbMuted ? "true":"false") + ",";
  j += "\"usbVolume\":" + String(gState.usbVolume) + ",";
  j += "\"usbMonitor\":" + String(gState.usbMonitor ? "true":"false") + ",";
  j += "\"usbPlaybackTransport\":" + String(gState.usbPlaybackTransport ? "true":"false") + ",";
  j += "\"aecEnabled\":" + String(gState.aecEnabled ? "true":"false") + ",";
  j += "\"usbSampleRate\":" + String(gState.usbSampleRate) + ",";
  j += "\"audioLoopback\":" + String(gState.audioLoopback ? "true":"false") + ",";
  j += "\"audioLevel\":{\"peak\":" + String(gState.audioPeak,3) +
       ",\"rms\":" + String(gState.audioRms,3) +
       ",\"clipped\":" + String(gState.audioClipped ? "true":"false") + "},";
  j += "\"ptt\":" + String(gState.ptt ? "true":"false") + ",";
  j += "\"sos\":" + String(gState.sos ? "true":"false") + ",";
  j += "\"recording\":" + String(gState.recording ? "true":"false") + ",";
  j += "\"rxActive\":" + String(gState.rxActive ? "true":"false") + ",";
  j += "\"recordingPaused\":" + String(gState.recordingPaused ? "true":"false") + ",\"playing\":" + String(gState.playing ? "true":"false") + ",\"playbackPaused\":" + String(gState.playbackPaused ? "true":"false") + ",\"playbackPositionMs\":" + String(gState.playbackPositionMs) + ",\"queueDepth\":" + String(gState.queueDepth) + ",\"vox\":" + String(gState.vox ? "true":"false") + ",\"voiceTxPackets\":" + String(gState.voiceTxPackets) + ",\"voiceRxPackets\":" + String(gState.voiceRxPackets) + ",\"voiceDrops\":" + String(gState.voiceDrops) + ",";
  j += "\"volume\":" + String(gState.volume) + ",";
  j += "\"runtimeAdvanced\":{\"mqttEnabled\":" + String(config.mqttEnabled ? "true" : "false") +
       ",\"wakePeriodSec\":" + String(config.wakePeriodSec) +
       ",\"staSsid\":\"" + jsonEscape(config.staSsid) + "\"" +
       ",\"staConfigured\":" + String(config.staSsid.length() > 0 ? "true" : "false") +
       ",\"deepSleepEnabled\":" + String(config.deepSleepEnabled ? "true" : "false") +
       ",\"deepSleepIdleMs\":" + String(config.deepSleepIdleMs) +
       ",\"deepSleepWakeGraceMs\":" + String(config.deepSleepWakeGraceMs) +
       ",\"criticalShutdownDelayMs\":" + String(config.criticalShutdownDelayMs) +
       ",\"batteryLowThreshold\":" + String(config.batteryLowThreshold, 3) +
       ",\"batteryCriticalThreshold\":" + String(config.batteryCriticalThreshold, 3) +
       ",\"classDEnabled\":" + String(config.classDEnabled ? "true" : "false") +
       ",\"classDBoostLevel\":" + String(config.classDBoostLevel) +
       ",\"mqttTlsMandatory\":" + String(Config::mqttTlsIsMandatory() ? "true" : "false") +
       ",\"classDHardwareEnabled\":" + String(Config::CLASS_D_ENABLED ? "true" : "false") +
       ",\"mqttHost\":\"" + jsonEscape(config.mqttHost) + "\"" +
       ",\"mqttPort\":" + String(config.mqttPort) +
       ",\"mqttTlsRequired\":" + String(config.mqttTlsRequired ? "true" : "false") +
       ",\"mqttReconnectMinMs\":" + String(config.mqttReconnectMinMs) +
       ",\"mqttReconnectMaxMs\":" + String(config.mqttReconnectMaxMs) +
       ",\"mqttTelemetryPeriodMs\":" + String(config.mqttTelemetryPeriodMs) +
       ",\"mqttHealthPeriodMs\":" + String(config.mqttHealthPeriodMs) +
       ",\"mqttRetainTelemetry\":" + String(config.mqttRetainTelemetry ? "true" : "false") +
       ",\"mqttRetainAvailability\":" + String(config.mqttRetainAvailability ? "true" : "false") +
       ",\"mqttCredentialRotationDays\":" + String(config.mqttCredentialRotationDays) +
       ",\"voxEnabled\":" + String(config.voxEnabled ? "true" : "false") +
       ",\"voxThreshold\":" + String(config.voxThreshold, 4) +
       ",\"voxHangMs\":" + String(config.voxHangMs) +
       ",\"aecEnabled\":" + String(config.aecEnabled ? "true" : "false") +
       ",\"usbMonitor\":" + String(config.usbMonitor ? "true" : "false") +
       ",\"usbPlaybackTransport\":" + String(config.usbPlaybackTransport ? "true" : "false") +
       ",\"audioLoopback\":" + String(config.audioLoopback ? "true" : "false") +
       ",\"loraAdrEnabled\":" + String(config.loraAdrEnabled ? "true" : "false") +
       ",\"loraHopEnabled\":" + String(config.loraHopEnabled ? "true" : "false") +
       ",\"loraHopChannelProfile\":" + String(config.loraHopChannelProfile) +
       ",\"loraRangeTestMode\":" + String(config.loraRangeTestMode ? "true" : "false") +
       ",\"sensorReaderEnabled\":" + String(config.sensorReaderEnabled ? "true" : "false") +
       ",\"sensorScanIntervalMs\":" + String(config.sensorScanIntervalMs) +
       ",\"sensorScanWindowMs\":" + String(config.sensorScanWindowMs) +
       ",\"sensorScanDurationMs\":" + String(config.sensorScanDurationMs) +
       ",\"sensorConnectTimeoutMs\":" + String(config.sensorConnectTimeoutMs) +
       ",\"sensorNodeEvictionMs\":" + String(config.sensorNodeEvictionMs) +
       ",\"sensorMaxNodes\":" + String(config.sensorMaxNodes) +
       ",\"sensorRequireEncryption\":" + String(config.sensorRequireEncryption ? "true" : "false") +
       ",\"blePairingEnabled\":" + String(config.blePairingEnabled ? "true" : "false") +
       ",\"blePairingFailureThreshold\":" + String(config.blePairingFailureThreshold) +
       ",\"blePairingBlockMs\":" + String(config.blePairingBlockMs) +
       ",\"sensorKeepAwake\":" + String(config.sensorKeepAwake ? "true" : "false") +
       ",\"webSessionTimeoutMs\":" + String(config.webSessionTimeoutMs) +
       ",\"webAuthRateLimitMs\":" + String(config.webAuthRateLimitMs) +
       ",\"csrfPolicy\":" + String(config.csrfPolicy) +
       ",\"blePairingPolicy\":" + String(config.blePairingPolicy) +
       ",\"ecdhRekeyPolicy\":" + String(config.ecdhRekeyPolicy) +
       ",\"replayWindowBits\":" + String(config.replayWindowBits) + "},";
  j += "\"voiceRxLost\":" + String(gState.voiceRxLost) + ",";
  j += "\"messageHistory\":" + String(gState.messageHistoryCount) + ",\"messageUnread\":" + String(gState.messageUnreadCount) + ",\"sosEscalated\":" + String(gState.sosEscalated ? "true" : "false") + ",";
  j += "\"loraLog\":" + String(gState.loraPacketLogCount) + ",";
  j += "\"healthAlerts\":" + String(gState.healthAlerts) + ",";
  j += "\"diagnostics\":{\"bootCount\":" + String(gState.bootCount) +
       ",\"wakeupCause\":" + String(gState.wakeupCause) +
       ",\"resetReason\":" + String(gState.resetReason) +
       ",\"brownout\":" + String(gState.brownoutReset ? "true" : "false") +
       ",\"heapLargestFree\":" + String(gState.heapLargestFree) +
       ",\"jammingDetected\":" + String(gState.jammingDetected ? "true" : "false") +
       ",\"noiseFloorDbm\":" + String(gState.noiseFloorDbm) +
       ",\"channelOccupancy\":" + String(gState.channelOccupancy) +
       ",\"antennaOk\":" + String(gState.antennaOk ? "true" : "false") +
       ",\"txRssi\":" + String(gState.txRssi) +
       ",\"antennaBaselineRssi\":" + String(gState.antennaBaselineRssi) + "},";
  j += "\"cpuTempC\":" + String(gState.cpuTempC, 1) +
       ",\"rangeTest\":" + String(gState.rangeTest ? "true" : "false") +
       ",\"batteryCalibrationDrift\":" +
       String(gState.batteryCalibrationDrift ? "true" : "false") + ",";
  j += "\"tx\":" + String(gState.txPackets) + ",";
  j += "\"rx\":" + String(gState.rxPackets) + ",";
  j += "\"msg\":\"" + jsonEscape(gState.lastMessage) + "\",";
  j += "\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += ",\"radioStats\":{\"forwardQueued\":" + String(lora.forwardQueued()) +
       ",\"forwardDrops\":" + String(lora.forwardDrops()) +
       ",\"forwardLastDropMs\":" + String(lora.forwardLastDropMs()) +
       ",\"fragmentEvictions\":" + String(lora.fragmentEvictions()) +
       ",\"fragmentDrops\":" + String(lora.fragmentDrops()) +
       ",\"dutyBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyBudgetUs())) +
       ",\"dutyMaxBudgetUs\":" + String(static_cast<unsigned long long>(lora.dutyMaxBudgetUs())) +
       ",\"gzipStalls\":" + String(storage.gzipStalls()) + "}";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Handlers: LoRaWAN
// ---------------------------------------------------------------------------
void WebUi::handleLoRaWANStatus(HttpdRequest& req, HttpdResponse& res) {
  (void)req;
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "{\"enabled\":" + String(config.lorawanEnabled ? "true" : "false") +
             ",\"mode\":" + String(config.lorawanMode) +
             ",\"region\":" + String(static_cast<uint8_t>(lorawan.regionalProfile())) +
             ",\"state\":" + String(static_cast<uint8_t>(lorawan.state())) +
             ",\"joined\":" + String(lorawan.isJoined() ? "true" : "false") +
             ",\"joining\":" + String(lorawan.isJoining() ? "true" : "false") +
             ",\"rssi\":" + String(lorawan.lastRssi()) +
             ",\"snr\":" + String(lorawan.lastSnr(), 1) +
             ",\"uplinkCount\":" + String(lorawan.uplinkCount()) +
             ",\"downlinkCount\":" + String(lorawan.downlinkCount()) +
             ",\"joinRetryCount\":" + String(lorawan.joinRetryCount()) +
             ",\"lastJoinMs\":" + String(lorawan.lastJoinAttemptMs()) +
             ",\"devEuiMasked\":\"" + jsonEscape(gState.lorawanDevEuiMasked) +
             "\",\"error\":\"" + jsonEscape(lorawan.lastError()) + "\"}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleLoRaWANConnect(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const bool ok = config.lorawanMode == 0 ? lorawan.connectOTAA() : lorawan.connectABP();
  res.sendText(ok ? 202 : 400, ok ? "LoRaWAN connect requested" : "LoRaWAN connect rejected");
}

void WebUi::handleLoRaWANDisconnect(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  const bool ok = lorawan.disconnect();
  res.sendText(ok ? 202 : 400, ok ? "LoRaWAN disconnect requested" : "LoRaWAN disconnect rejected");
}

void WebUi::handleLoRaWANConfig(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastConfigMs_, config.webAuthRateLimitMs)) return;
  RuntimeConfig candidate;
  uint32_t configGenerationSnapshot = 0;
  if (!configSnapshot(candidate, configGenerationSnapshot)) {
    res.send503("configuration busy");
    return;
  }

  auto hexField = [](const String& v, size_t n) {
    if (v.length() != n) return false;
    for (size_t i = 0; i < v.length(); ++i) {
      const char c = v[i];
      if (!isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
  };
  auto parseU32 = [](const String& raw, uint32_t maxValue, uint32_t& out) {
    if (raw.isEmpty() || raw.length() > 10) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < raw.length(); ++i) {
      if (raw[i] < '0' || raw[i] > '9') return false;
      const uint32_t digit = static_cast<uint32_t>(raw[i] - '0');
      if (digit > maxValue || value > (maxValue - digit) / 10U) return false;
      value = value * 10U + digit;
    }
    out = value;
    return true;
  };

  if (req.hasArg("enabled")) {
    const String v = req.arg("enabled");
    if (v != "0" && v != "1") { res.sendText(400, "invalid enabled"); return; }
    candidate.lorawanEnabled = v == "1";
  }
  if (req.hasArg("mode")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("mode"), 1, v)) { res.sendText(400, "invalid mode"); return; }
    candidate.lorawanMode = static_cast<uint8_t>(v);
  }
  if (req.hasArg("region")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("region"), 3, v)) { res.sendText(400, "invalid region"); return; }
    candidate.lorawanRegion = static_cast<uint8_t>(v);
  }
  if (req.hasArg("deveui")) candidate.lorawanDevEui = req.arg("deveui");
  if (req.hasArg("joineui")) candidate.lorawanJoinEui = req.arg("joineui");
  if (req.hasArg("appkey")) candidate.lorawanAppKey = req.arg("appkey");
  if (req.hasArg("nwkskey")) candidate.lorawanNwkSKey = req.arg("nwkskey");
  if (req.hasArg("appskey")) candidate.lorawanAppSKey = req.arg("appskey");

  if (req.hasArg("devaddr")) {
    const String raw = req.arg("devaddr");
    if (!hexField(raw, 8)) { res.sendText(400, "invalid DevAddr"); return; }
    for (size_t i = 0; i < 4; ++i) {
      auto n = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = n(raw[i * 2]), lo = n(raw[i * 2 + 1]);
      if (hi < 0 || lo < 0) { res.sendText(400, "invalid DevAddr"); return; }
      candidate.lorawanDevAddr[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
  }
  if (req.hasArg("fport")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("fport"), 223, v) || v == 0) {
      res.sendText(400, "invalid FPort"); return;
    }
    candidate.lorawanFPort = static_cast<uint8_t>(v);
  }
  if (req.hasArg("period")) {
    uint32_t v = 0;
    if (!parseU32(req.arg("period"), 86400, v) || v == 0) {
      res.sendText(400, "invalid period"); return;
    }
    candidate.lorawanUplinkPeriodSec = static_cast<uint16_t>(min<uint32_t>(v, 65535U));
  }

  if (candidate.lorawanEnabled) {
    if (!hexField(candidate.lorawanDevEui, 16) ||
        (candidate.lorawanMode == 0 &&
         (!hexField(candidate.lorawanJoinEui, 16) || !hexField(candidate.lorawanAppKey, 32))) ||
        (candidate.lorawanMode == 1 &&
         (!hexField(candidate.lorawanNwkSKey, 32) || !hexField(candidate.lorawanAppSKey, 32)))) {
      res.sendText(400, "invalid LoRaWAN credentials"); return;
    }
  }
  if (!candidate.validLoRaWAN()) {
    res.sendText(400, "invalid LoRaWAN configuration"); return;
  }

  RuntimeConfig previous;
  if (!configSnapshot(previous)) {
    res.send503("configuration busy");
    return;
  }
  const bool wasJoined = lorawan.isJoined();
  if (wasJoined) (void)lorawan.disconnect();
  if (!configCommit(candidate, configGenerationSnapshot)) {
    res.sendText(409, "configuration changed; retry");
    return;
  }
  if (candidate.lorawanRegion != previous.lorawanRegion)
    lorawan.setRegionalProfile(static_cast<RegionalProfile>(candidate.lorawanRegion));
  if (!candidate.lorawanEnabled) (void)lorawan.disconnect();
  auditConfigChange(previous, candidate, "lorawan-web");
  res.sendText(200, "LoRaWAN configuration saved");
}

void WebUi::handleLoRaWANUplink(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastMessageMs_, config.webAuthRateLimitMs)) return;
  const String text = req.arg("text");
  const String raw = req.arg("hex");
  uint8_t payload[Config::LORAWAN_MAX_PAYLOAD] = {};
  size_t len = 0;
  if (!raw.isEmpty()) {
    if ((raw.length() & 1U) || raw.length() > Config::LORAWAN_MAX_PAYLOAD * 2U) {
      res.sendText(400, "invalid hex payload"); return;
    }
    auto n = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i < raw.length() / 2; ++i) {
      const int hi = n(raw[i * 2]), lo = n(raw[i * 2 + 1]);
      if (hi < 0 || lo < 0) { res.sendText(400, "invalid hex payload"); return; }
      payload[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    len = raw.length() / 2;
  } else {
    if (text.isEmpty() || text.length() > Config::LORAWAN_MAX_PAYLOAD) {
      res.sendText(400, "invalid text payload"); return;
    }
    memcpy(payload, text.c_str(), text.length());
    len = text.length();
  }
  const String confirmed = req.arg("confirmed");
  const bool isConfirmed = confirmed == "1";
  const bool ok = lorawan.sendUplink(config.lorawanFPort, payload, len, isConfirmed);
  res.sendText(ok ? 202 : 409, ok ? "uplink queued" : "uplink rejected");
}

// ---------------------------------------------------------------------------
// Handlers: files
// ---------------------------------------------------------------------------
void WebUi::handleFiles(HttpdRequest& req, HttpdResponse& res) {
  const String dir = req.arg("dir");
  res.sendJson(200, storage.listJson(dir.isEmpty() ? "/REC" : dir));
}

void WebUi::handleDownload(HttpdRequest& req, HttpdResponse& res) {
  const String path = req.arg("path");
  if (!storage.isManagedAudioPath(path)) {
    res.sendText(400, "invalid path");
    return;
  }
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { res.send503("busy"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    res.send404("not found");
    return;
  }
  res.setHeader("Content-Disposition", "attachment; filename=\"" +
                path.substring(path.lastIndexOf('/') + 1) + "\"");
  if (!res.streamFile(f, "audio/wav")) {
    Serial.println("WebUI: streamFile failed");
  }
  f.close();
}

void WebUi::handleUpload(HttpdRequest& req, HttpdResponse& res) {
  uploadFailed_ = false;
  uploadBytes_ = 0;
  uploadPath_ = String();

  HttpdMultipart parser;
  auto fieldCb = [](const char*, const char*, const uint8_t*, size_t) -> bool {
    return true;
  };
  auto fileCb = [this](const char* name, const char* filename, const char* mime,
                       const uint8_t* data, size_t len,
                       bool firstChunk, bool lastChunk) -> bool {
    handleUploadChunk(name, filename, mime, data, len, firstChunk, lastChunk);
    return !uploadFailed_;
  };

  if (!parser.parse(req, Config::WEB_UPLOAD_MAX_BYTES + 4096, fieldCb, fileCb)) {
    uploadFailed_ = true;
  }
  if (uploadFailed_) {
    res.sendText(400, "upload failed");
    return;
  }
  res.sendText(200, "OK");
}

void WebUi::handleUploadChunk(const char* /*name*/, const char* filename,
                              const char* /*mime*/, const uint8_t* data,
                              size_t len, bool firstChunk, bool lastChunk) {
  if (uploadFailed_) return;

  if (firstChunk) {
    String name = filename ? String(filename) : String();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    uploadPath_ = "/REC/" + name;
    uploadBytes_ = 0;
    if (!storage.isManagedAudioPath(uploadPath_) || SD.exists(uploadPath_)) {
      uploadFailed_ = true;
      return;
    }
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (!spiLock.ok()) { uploadFailed_ = true; return; }
    uploadFile_ = SD.open(uploadPath_, FILE_WRITE);
    if (!uploadFile_) uploadFailed_ = true;
    return;
  }

  if (data != nullptr && len > 0) {
    if (uploadFailed_ || !uploadFile_ ||
        len > Config::WEB_UPLOAD_MAX_BYTES - uploadBytes_) {
      uploadFailed_ = true;
      return;
    }
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (!spiLock.ok() || uploadFile_.write(data, len) != len) {
      uploadFailed_ = true;
      return;
    }
    uploadBytes_ += len;
  }

  if (lastChunk) {
    if (uploadFile_) uploadFile_.close();
    if (!uploadFailed_ &&
        (uploadBytes_ == 0 || uploadBytes_ > Config::WEB_UPLOAD_MAX_BYTES ||
         !isValidUploadedWav(uploadPath_))) {
      uploadFailed_ = true;
    }
    if (uploadFailed_ && !uploadPath_.isEmpty()) {
      SpiLock spiLock(pdMS_TO_TICKS(100));
      if (spiLock.ok()) SD.remove(uploadPath_);
    }
  }
}

void WebUi::handleRename(HttpdRequest& req, HttpdResponse& res) {
  const String from = req.arg("from");
  const String to = req.arg("to");
  const bool ok = storage.renameFile(from, to);
  res.sendText(ok ? 200 : 400, ok ? "OK" : "FAIL");
}

// ---------------------------------------------------------------------------
// Handlers: messages
// ---------------------------------------------------------------------------
void WebUi::handleMessages(HttpdRequest& req, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  const String query = req.arg("q");
  uint64_t from = 0, to = UINT64_MAX;
  if (req.hasArg("from")) from = strtoull(req.arg("from").c_str(), nullptr, 10);
  if (req.hasArg("to")) to = strtoull(req.arg("to").c_str(), nullptr, 10);
  String j = "[";
  bool first = true;
  const size_t count = gState.messageHistoryCount;
  const size_t start = (gState.messageHistoryNext + Config::MESSAGE_HISTORY_SIZE - count) %
                       Config::MESSAGE_HISTORY_SIZE;
  for (size_t i = 0; i < count; ++i) {
    const auto& e = gState.messageHistory[(start + i) % Config::MESSAGE_HISTORY_SIZE];
    if (e.timestamp < from || e.timestamp > to) continue;
    if (!query.isEmpty() &&
        String(e.sourceId).indexOf(query) < 0 &&
        e.text.indexOf(query) < 0) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
         ",\"source\":" + String(e.sourceId) +
         ",\"read\":" + String(e.read ? "true" : "false") +
         ",\"text\":\"" + jsonEscape(e.text) + "\"}";
  }
  j += "]";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleMessageClear(HttpdRequest& /*req*/, HttpdResponse& res) {
  {
    StateLock lock(gState);
    if (!lock.ok()) { res.send503("busy"); return; }
    for (auto& e : gState.messageHistory) e = MessageHistoryEntry{};
    gState.messageHistoryNext = 0;
    gState.messageHistoryCount = 0;
    gState.messageUnreadCount = 0;
  }
  (void)lora.persistMessageHistory();
  res.sendText(200, "OK");
}

void WebUi::handleMessageRead(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("ts");
  const bool all = raw == "all";
  if (!all && (raw.isEmpty() || raw.length() > 20)) {
    res.sendText(400, "invalid timestamp"); return;
  }
  uint64_t ts = all ? 0 : strtoull(raw.c_str(), nullptr, 10);
  {
    StateLock lock(gState);
    if (!lock.ok()) { res.send503("busy"); return; }
    for (auto& e : gState.messageHistory) {
      if (e.timestamp != 0 && (all || e.timestamp == ts) && !e.read) {
        e.read = true;
        if (gState.messageUnreadCount) --gState.messageUnreadCount;
      }
    }
  }
  (void)lora.persistMessageHistory();
  res.sendText(200, "OK");
}

void WebUi::handleMessageReply(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastMessageMs_, config.webAuthRateLimitMs)) return;
  const String target = req.arg("source");
  String text;
  if (!req.bodyText(text, Config::LORA_FRAGMENT_MAX_BYTES)) {
    res.sendText(413, "reply too large"); return;
  }
  if (target.isEmpty() || target.length() > 10 || text.isEmpty() ||
      text.length() > Config::LORA_FRAGMENT_MAX_BYTES) {
    res.sendText(400, "invalid reply"); return;
  }

  uint32_t destination = 0;
  for (size_t i = 0; i < target.length(); ++i) {
    if (target[i] < '0' || target[i] > '9') {
      res.sendText(400, "invalid source"); return;
    }
    const uint32_t digit = static_cast<uint32_t>(target[i] - '0');
    if (destination > (UINT32_MAX - digit) / 10U) {
      res.sendText(400, "invalid source"); return;
    }
    destination = destination * 10U + digit;
  }
  if (destination == 0) {
    res.sendText(400, "invalid source"); return;
  }
  const bool ok = lora.sendTextTo(destination, text);
  res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
}

void WebUi::handleMessageExport(HttpdRequest& req, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  const String format = req.arg("format");
  String out;
  if (format.equalsIgnoreCase("csv")) {
    out = "timestamp,source,read,text\n";
    const size_t count = gState.messageHistoryCount;
    const size_t start = (gState.messageHistoryNext + Config::MESSAGE_HISTORY_SIZE - count) %
                         Config::MESSAGE_HISTORY_SIZE;
    for (size_t i = 0; i < count; ++i) {
      const auto& e = gState.messageHistory[(start + i) % Config::MESSAGE_HISTORY_SIZE];
      String text = e.text;
      text.replace("\"", "\"\"");
      out += String(static_cast<unsigned long long>(e.timestamp)) + "," +
             String(e.sourceId) + "," + (e.read ? "1" : "0") + ",\"" + text + "\"\n";
    }
    res.setHeader("Content-Disposition", "attachment; filename=\"messages.csv\"");
    res.sendCsv(200, out);
    return;
  }
  out = "[";
  const size_t count = gState.messageHistoryCount;
  const size_t start = (gState.messageHistoryNext + Config::MESSAGE_HISTORY_SIZE - count) %
                       Config::MESSAGE_HISTORY_SIZE;
  for (size_t i = 0; i < count; ++i) {
    if (i) out += ",";
    const auto& e = gState.messageHistory[(start + i) % Config::MESSAGE_HISTORY_SIZE];
    out += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
           ",\"source\":" + String(e.sourceId) +
           ",\"read\":" + String(e.read ? "true" : "false") +
           ",\"text\":\"" + jsonEscape(e.text) + "\"}";
  }
  out += "]";
  res.setHeader("Content-Disposition", "attachment; filename=\"messages.json\"");
  res.sendJson(200, out);
}

void WebUi::handleMessage(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastMessageMs_, config.webAuthRateLimitMs)) return;
  if (req.contentLength() > Config::MAX_WEB_BODY) {
    res.send413("payload too large");
    return;
  }
  String text;
  if (!req.bodyText(text, Config::MAX_WEB_BODY) || text.isEmpty() ||
      text.length() > Config::LORA_MAX_PACKET) {
    res.sendText(400, "invalid payload"); return;
  }
  const bool ok = lora.sendText(text);
  const bool acked = lora.textAcked();
  res.sendJson(ok ? 200 : 503,
               "{\"sent\":" + String(ok ? "true" : "false") +
               ",\"acked\":" + String(acked ? "true" : "false") + "}");
}

void WebUi::handleMessagePersist(HttpdRequest& /*req*/, HttpdResponse& res) {
  const bool ok = lora.persistMessageHistory();
  res.sendText(ok ? 200 : 503, ok ? "OK" : "persist failed");
}

// ---------------------------------------------------------------------------
// Handlers: record/message schedule
// ---------------------------------------------------------------------------
void WebUi::handleMessageSchedule(HttpdRequest& req, HttpdResponse& res) {
  if (!req.hasArg("at") || !req.hasArg("text")) {
    res.sendText(400, "at and text required"); return;
  }
  const String a = req.arg("at");
  char* end = nullptr;
  const unsigned long long at = strtoull(a.c_str(), &end, 10);
  const String text = req.arg("text");
  if (!end || *end != '\0' || at == 0 || text.isEmpty()) {
    res.sendText(400, "invalid schedule"); return;
  }
  const bool ok = lora.scheduleMessage(static_cast<uint64_t>(at), text);
  res.sendText(ok ? 200 : 503, ok ? "OK" : "schedule full");
}

void WebUi::handleMessageScheduleList(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, lora.scheduledMessagesJson());
}

void WebUi::handleMessageScheduleDelete(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("id");
  if (raw.isEmpty() || raw.length() > 10) {
    res.sendText(400, "invalid id"); return;
  }
  char* end = nullptr;
  const unsigned long value = strtoul(raw.c_str(), &end, 10);
  if (!end || *end != '\0' || value == 0 || value > UINT32_MAX) {
    res.sendText(400, "invalid id"); return;
  }
  const uint32_t id = static_cast<uint32_t>(value);
  res.sendText(lora.cancelScheduledMessage(id) ? 200 : 404, "OK");
}

void WebUi::handleRecordSchedule(HttpdRequest& req, HttpdResponse& res) {
  if (!req.hasArg("start") || !req.hasArg("duration")) {
    res.sendText(400, "start and duration required"); return;
  }
  char* startEnd = nullptr;
  char* durationEnd = nullptr;
  const String rawStart = req.arg("start");
  const String rawDuration = req.arg("duration");
  if (rawStart.isEmpty() || rawStart.length() > 20 ||
      rawDuration.isEmpty() || rawDuration.length() > 10) {
    res.sendText(400, "invalid schedule"); return;
  }
  const unsigned long long start = strtoull(rawStart.c_str(), &startEnd, 10);
  const unsigned long duration = strtoul(rawDuration.c_str(), &durationEnd, 10);
  if (!startEnd || *startEnd != '\0' ||
      !durationEnd || *durationEnd != '\0' ||
      start == 0 || duration == 0 || duration > Config::RECORD_MAX_SECONDS) {
    res.sendText(400, "invalid schedule"); return;
  }
  recordScheduleStart_ = static_cast<uint64_t>(start);
  recordScheduleDurationSec_ = static_cast<uint32_t>(duration);
  recordScheduleActive_ = true;
  res.sendText(200, "OK");
}

void WebUi::handleRecordScheduleGet(HttpdRequest& /*req*/, HttpdResponse& res) {
  String j = "{\"active\":" + String(recordScheduleActive_ ? "true" : "false") +
             ",\"start\":" + String(static_cast<unsigned long long>(recordScheduleStart_)) +
             ",\"duration\":" + String(recordScheduleDurationSec_) + "}";
  res.sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Handlers: SOS
// ---------------------------------------------------------------------------
void WebUi::handleSosFormat(HttpdRequest& req, HttpdResponse& res) {
  const String raw = req.arg("list");
  if (raw.isEmpty() || raw.length() > 32) {
    res.sendText(400, "invalid format list"); return;
  }
  uint8_t mask = 0;
  int start = 0;
  while (start < static_cast<int>(raw.length())) {
    int comma = raw.indexOf(',', start);
    if (comma < 0) comma = raw.length();
    String item = raw.substring(start, comma);
    item.trim();
    if (item == "text") mask |= 1;
    else if (item == "aprs") mask |= 2;
    else if (item == "binary") mask |= 4;
    else { res.sendText(400, "unknown format"); return; }
    start = comma + 1;
  }
  res.sendText(lora.setSosFormats(mask) ? 200 : 400, "OK");
}

void WebUi::handleSos(HttpdRequest& req, HttpdResponse& res) {
  if (!rateLimit(req, res, lastSosMs_, Config::SOS_RATE_LIMIT_MS)) return;
  const String raw = req.arg("on");
  if (raw == "0") {
    if (!lora.cancelSOS()) {
      res.send503("SOS cancel failed");
      return;
    }
    res.sendText(200, "SOS OFF");
    return;
  }
  if (raw.isEmpty() || raw != "1") {
    res.sendText(400, "invalid sos");
    return;
  }
  bool ok = lora.sendSOS();
  if (ok) {
    StateLock lock(gState);
    if (lock.ok()) gState.sos = true;
  }
  res.sendText(ok ? 200 : 503, ok ? "SOS" : "FAIL");
}

void WebUi::handleSosStatus(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "{";
  j += "\"seq\":" + String(gState.sosSeq);
  j += ",\"acked\":" + String(gState.sosAcked ? "true" : "false");
  j += ",\"retries\":" + String(gState.sosRetries);
  j += ",\"lastAckMs\":" + String(gState.sosLastAckMs);
  j += ",\"source\":" + String(gState.sosLastAckSourceId);
  j += ",\"active\":" + String(gState.sos ? "true" : "false");
  j += ",\"escalated\":" + String(gState.sosEscalated ? "true" : "false");
  j += ",\"beacons\":" + String(gState.sosBeaconCount);
  j += ",\"ackedBy\":" + String(gState.sosAckedBy);
  j += ",\"error\":\"" + jsonEscape(gState.lastError) + "\"";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleSosHistory(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.send503("busy"); return; }
  String j = "[";
  const size_t count = gState.sosHistoryCount;
  const size_t start = (gState.sosHistoryNext + RuntimeState::SOS_HISTORY_SIZE - count) %
                       RuntimeState::SOS_HISTORY_SIZE;
  for (size_t i = 0; i < count; ++i) {
    const auto& e = gState.sosHistory[(start + i) % RuntimeState::SOS_HISTORY_SIZE];
    if (i) j += ",";
    j += "{\"ts\":" + String(static_cast<unsigned long long>(e.timestamp)) +
         ",\"seq\":" + String(e.seq) + ",\"event\":" + String(e.event) +
         ",\"peer\":" + String(e.peer) + "}";
  }
  j += "]";
  res.sendJson(200, j);
}

// ---------------------------------------------------------------------------
// Handlers: selftest
// ---------------------------------------------------------------------------
void WebUi::handleSelfTest(HttpdRequest& /*req*/, HttpdResponse& res) {
  bool loraOk = false, codecOk = false, sdOk = false, gpsOk = false, batOk = false;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      loraOk = gState.loraReady;
      codecOk = gState.codecReady;
      sdOk = gState.storageReady;
      gpsOk = gState.gps.valid;
      batOk = gState.batteryAvailable;
    }
  }
  const bool pass = loraOk && codecOk && sdOk;
  selfTestMs_ = millis();
  selfTestResult_ = "{\"pass\":" + String(pass ? "true" : "false") +
    ",\"lora\":" + String(loraOk ? "true" : "false") +
    ",\"audio\":" + String(codecOk ? "true" : "false") +
    ",\"sd\":" + String(sdOk ? "true" : "false") +
    ",\"gps\":" + String(gpsOk ? "true" : "false") +
    ",\"battery\":" + String(batOk ? "true" : "false") + "}";
  res.sendJson(200, selfTestResult_);
}

void WebUi::handleSelfTestResult(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.sendJson(200, selfTestResult_.isEmpty()
                       ? "{\"pass\":false,\"error\":\"not run\"}"
                       : selfTestResult_);
}

// ---------------------------------------------------------------------------
// Handlers: UI prefs
// ---------------------------------------------------------------------------
void WebUi::handleLang(HttpdRequest& req, HttpdResponse& res) {
  const String code = req.arg("set");
  if (code != "en" && code != "id") {
    res.sendText(400, "invalid language"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) {
    res.send503("NVS unavailable"); return;
  }
  const bool ok = prefs.putString("lang", code) > 0;
  prefs.end();
  res.sendText(ok ? 200 : 503, ok ? "OK" : "NVS save failed");
}

void WebUi::handleTheme(HttpdRequest& req, HttpdResponse& res) {
  const String mode = req.arg("mode");
  if (mode != "dark" && mode != "light") {
    res.sendText(400, "invalid theme"); return;
  }
  Preferences prefs;
  if (!prefs.begin("fieldradio", false)) {
    res.send503("NVS unavailable"); return;
  }
  const bool ok = prefs.putString("theme", mode) > 0;
  prefs.end();
  res.sendText(ok ? 200 : 503, ok ? "OK" : "NVS save failed");
}

// === SAMBUNG KE PESAN 6b ===
```

---

Status Setelah Pesan 6a

WebUi.cpp sekarang berisi Bagian 1 + Bagian 2 + Bagian 3a:

Handler yang sudah diimplementasikan (30 dari 121):

· Root & meta: handleRoot, handleApiVersion, handleStatus
· LoRaWAN: handleLoRaWANStatus, handleLoRaWANConnect, handleLoRaWANDisconnect, handleLoRaWANConfig, handleLoRaWANUplink
· Files: handleFiles, handleDownload, handleUpload, handleUploadChunk, handleRename
· Messages: handleMessages, handleMessageClear, handleMessageRead, handleMessageReply, handleMessageExport, handleMessage, handleMessagePersist
· Schedule: handleMessageSchedule, handleMessageScheduleList, handleMessageScheduleDelete, handleRecordSchedule, handleRecordScheduleGet
· SOS: handleSosFormat, handleSos, handleSosStatus, handleSosHistory
· Selftest: handleSelfTest, handleSelfTestResult
· UI: handleLang, handleTheme

Belum ada (91 handler): diagnostics, capture, ADR, HOP, scanner, range-test, radio, battery, storage, audio (PTT/record/play/vox/volume/audio-source/audio-monitor/audio-loopback/audio-aec/audio-tone/usb-transport/queue/delete/seek/pause/record-pause/record-split/vad/record-quality), track, sensor, BLE passkey, MQTT, config, factory-reset, reboot.

Kalau Anda build sekarang, akan error undefined reference untuk 91 handler itu.

---

Konfirmasi

Setelah tersalin, konfirmasi. Saya lanjut ke Pesan 6b: 50 handler diagnostics/capture/ADR/HOP/scanner/range-test/radio/battery/storage/audio.