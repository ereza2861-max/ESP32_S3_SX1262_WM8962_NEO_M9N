Iterasi C — Pesan 6c dari 7: WebUi.cpp Bagian 3c (Terakhir)

Sambung ke File yang Sama

Hapus baris // === SAMBUNG KE PESAN 6c ===, lalu salin blok di bawah ini. Setelah ini, WebUi.cpp selesai.

---

```cpp
// ---------------------------------------------------------------------------
// Helpers: sensor id, BLE address
// ---------------------------------------------------------------------------
bool WebUi::parseSensorNodeId(HttpdRequest& req, size_t& id) {
  const String raw = req.arg("id");
  if (raw.isEmpty()) return false;
  const long value = raw.toInt();
  if (value < 0 || value >= static_cast<long>(SensorRegistry::MAX_SUPPORTED_NODES)) return false;
  id = static_cast<size_t>(value);
  return true;
}

bool WebUi::parseBleAddressArg(HttpdRequest& req, SensorProtocol::BleAddress& out) {
  const String raw = req.arg("addr");
  if (raw.length() != 17) return false;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < 6; ++i) {
    const size_t pos = (5U - i) * 3U;
    if (i < 5 && raw[pos + 2] != ':') return false;
    const int hi = hex(raw[pos]), lo = hex(raw[pos + 1]);
    if (hi < 0 || lo < 0) return false;
    out.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  out.type = 0;
  return true;
}

// ---------------------------------------------------------------------------
// Handlers: track / GPS
// ---------------------------------------------------------------------------
void WebUi::handleTrack(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) { res.sendEmpty(503); return; }
  String j = "{\"valid\":" + String(gState.gps.valid ? "true":"false") +
             ",\"lat\":" + String(gState.gps.lat,6) +
             ",\"lon\":" + String(gState.gps.lon,6) + "}";
  res.sendJson(200, j);
}

void WebUi::handleTrackPoints(HttpdRequest& req, HttpdResponse& res) {
  uint64_t from = 0, to = UINT64_MAX;
  if (req.hasArg("from")) {
    const String raw = req.arg("from");
    if (raw.isEmpty() || raw.length() > 20) {
      res.sendText(400, "invalid from"); return;
    }
    char* end = nullptr;
    from = strtoull(raw.c_str(), &end, 10);
    if (!end || *end != '\0') {
      res.sendText(400, "invalid from"); return;
    }
  }
  if (req.hasArg("to")) {
    const String raw = req.arg("to");
    if (raw.isEmpty() || raw.length() > 20) {
      res.sendText(400, "invalid to"); return;
    }
    char* end = nullptr;
    to = strtoull(raw.c_str(), &end, 10);
    if (!end || *end != '\0') {
      res.sendText(400, "invalid to"); return;
    }
  }
  size_t limit = 1000;
  if (req.hasArg("limit")) {
    const String raw = req.arg("limit");
    if (raw.isEmpty() || raw.length() > 4) {
      res.sendText(400, "invalid limit"); return;
    }
    char* end = nullptr;
    const unsigned long v = strtoul(raw.c_str(), &end, 10);
    if (!end || *end != '\0' || v == 0) {
      res.sendText(400, "invalid limit"); return;
    }
    limit = min<unsigned long>(v, 5000UL);
  }
  const String j = storage.readTrackCsv(from, to, limit);
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleTrackSimplified(HttpdRequest& req, HttpdResponse& res) {
  double epsilon = 10.0;
  if (req.hasArg("epsilon")) {
    const String raw = req.arg("epsilon");
    if (raw.isEmpty() || raw.length() > 12) {
      res.sendText(400, "invalid epsilon"); return;
    }
    char* end = nullptr;
    epsilon = strtod(raw.c_str(), &end);
    if (!end || *end != '\0' || !isfinite(epsilon) || epsilon <= 0.0 || epsilon > 10000.0) {
      res.sendText(400, "invalid epsilon"); return;
    }
  }
  const String j = storage.readTrackCsvSimplified(0, UINT64_MAX, 5000, epsilon);
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleTrackDownload(HttpdRequest& /*req*/, HttpdResponse& res) {
  SpiLock spiLock(pdMS_TO_TICKS(200));
  if (!spiLock.ok()) { res.send503("busy"); return; }
  File f = SD.open("/TRACK/TRACK.CSV", FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    res.send404("track not found");
    return;
  }
  res.setHeader("Content-Disposition", "attachment; filename=TRACK.CSV");
  (void)res.streamFile(f, "text/csv");
  f.close();
}

// ---------------------------------------------------------------------------
// Handlers: BLE sensors
// ---------------------------------------------------------------------------
void WebUi::handleSensorNodes(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorNodesMs_, config.webAuthRateLimitMs)) return;
  size_t count = 0;
  if (!bleSensorReader.sensorReader().snapshotNodes(gSensorSnapshots,
                                                    SensorRegistry::MAX_SUPPORTED_NODES,
                                                    count)) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"sensor snapshot unavailable\"}");
    return;
  }
  String j = "{\"ok\":true,\"sensorDropped\":" +
             String(bleSensorReader.sensorReader().droppedSamples()) +
             ",\"queueDepth\":" + String(bleSensorReader.sensorReader().queueDepth()) +
             ",\"nodes\":[";
  for (size_t i = 0; i < count; ++i) {
    if (i) j += ',';
    j += sensorNodeJson(gSensorSnapshots[i].index, gSensorSnapshots[i].node, false);
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j + "]}");
}

void WebUi::handleSensorNodeDetail(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorNodesMs_, config.webAuthRateLimitMs)) return;
  const String raw = req.arg("id");
  if (raw.isEmpty()) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"missing id\"}");
    return;
  }
  const long id = raw.toInt();
  if (id < 0 || id >= static_cast<long>(SensorRegistry::MAX_SUPPORTED_NODES)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  SensorRegistry::Node node{};
  if (!bleSensorReader.sensorReader().snapshotNode(static_cast<size_t>(id), node)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"node not found\"}");
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, "{\"ok\":true," +
                    sensorNodeJson(static_cast<size_t>(id), node, true).substring(1));
}

void WebUi::handleSensorLive(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorLiveMs_, config.webAuthRateLimitMs)) return;
  size_t count = 0;
  if (!bleSensorReader.sensorReader().snapshotNodes(gSensorSnapshots,
                                                    SensorRegistry::MAX_SUPPORTED_NODES,
                                                    count)) {
    res.sendJson(503, "{\"ok\":false}");
    return;
  }
  String j = "{\"ok\":true,\"sensorDropped\":" +
             String(bleSensorReader.sensorReader().droppedSamples()) +
             ",\"queueDepth\":" + String(bleSensorReader.sensorReader().queueDepth()) +
             ",\"nodes\":[";
  for (size_t i = 0; i < count; ++i) {
    if (i) j += ',';
    j += sensorNodeJson(gSensorSnapshots[i].index, gSensorSnapshots[i].node, true);
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j + "]}");
}

void WebUi::handleSensorForget(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorActionMs_, config.webAuthRateLimitMs)) return;
  size_t id = 0;
  if (!parseSensorNodeId(req, id)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  if (!bleSensorReader.sensorReader().requestForgetNode(id)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"node not found\"}");
    return;
  }
  res.sendJson(202, "{\"ok\":true,\"queued\":true}");
}

void WebUi::handleSensorRefresh(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorActionMs_, config.webAuthRateLimitMs)) return;
  size_t id = 0;
  if (!parseSensorNodeId(req, id)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  if (!bleSensorReader.sensorReader().requestRefreshNode(id)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"node not found\"}");
    return;
  }
  res.sendJson(202, "{\"ok\":true,\"queued\":true}");
}

void WebUi::handleSensorQueuePolicy(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorQueuePolicyMs_, config.webAuthRateLimitMs)) return;
  const String policy = req.arg("policy");
  if (policy == "oldest")
    bleSensorReader.sensorReader().setQueuePolicy(SensorReader::SampleQueuePolicy::DROP_OLDEST);
  else if (policy == "newest")
    bleSensorReader.sensorReader().setQueuePolicy(SensorReader::SampleQueuePolicy::DROP_NEWEST);
  else {
    res.sendJson(400, "{\"ok\":false,\"error\":\"policy must be newest|oldest\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true,\"policy\":\"" + policy + "\"}");
}

void WebUi::handleSensorDedupStats(HttpdRequest& /*req*/, HttpdResponse& res) {
  StateLock lock(gState);
  if (!lock.ok()) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"state unavailable\"}");
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200,
               "{\"ok\":true,\"persistenceFailures\":" +
                   String(gState.sensorDedupFailures) +
                   ",\"sensorBatchAckRetries\":" + String(gState.sensorBatchAckRetries) +
                   ",\"sensorBatchAckFailures\":" + String(gState.sensorBatchAckFailures) +
                   ",\"remoteSensorOverflowDrops\":" + String(gState.remoteSensorOverflowDrops) + "}");
}

void WebUi::handleSensorSpool(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorNodesMs_, config.webAuthRateLimitMs)) return;
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, sensorSpool.statusJson());
}

void WebUi::handleSensorSpoolClear(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastSensorActionMs_, config.webAuthRateLimitMs)) return;
  if (!sensorSpool.clear()) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"spool clear failed\"}");
    return;
  }
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.sensorSpoolDepth = 0;
      gState.sensorSpoolEvictions = sensorSpool.evictions();
      gState.sensorSpoolDrops = sensorSpool.drops();
      gState.sensorSpoolRecovered = sensorSpool.recovered();
    }
  }
  res.sendJson(200, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Handlers: BLE passkey
// ---------------------------------------------------------------------------
void WebUi::handleBlePasskeySet(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastBlePasskeyMs_, config.webAuthRateLimitMs)) return;
  SensorProtocol::BleAddress address{};
  const String pass = req.arg("passkey");
  if (!parseBleAddressArg(req, address) || pass.length() != 6) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid addr/passkey\"}");
    return;
  }
  for (size_t i = 0; i < 6; ++i) {
    if (!isdigit(static_cast<unsigned char>(pass[i]))) {
      res.sendJson(400, "{\"ok\":false,\"error\":\"passkey must be 6 digits\"}");
      return;
    }
  }
  const uint32_t value = static_cast<uint32_t>(pass.toInt());
  if (value < 100000U || value > 999999U ||
      !bleSensorReader.setPeerPasskey(address, value)) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"passkey rejected\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true}");
}

void WebUi::handleBlePasskeyDelete(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastBlePasskeyMs_, config.webAuthRateLimitMs)) return;
  SensorProtocol::BleAddress address{};
  if (!parseBleAddressArg(req, address) ||
      !bleSensorReader.forgetPeerPasskey(address)) {
    res.sendJson(404, "{\"ok\":false,\"error\":\"peer not found\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true}");
}

void WebUi::handleBlePasskeyList(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (!rateLimit(req, res, lastBlePasskeyMs_, config.webAuthRateLimitMs)) return;
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, "{\"ok\":true,\"peers\":" + bleSensorReader.peersJson() + "}");
}

// ---------------------------------------------------------------------------
// Handlers: MQTT
// ---------------------------------------------------------------------------
void WebUi::handleMqttProvision(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  static uint32_t lastMqttProvisionMs = 0;
  if (!rateLimit(req, res, lastMqttProvisionMs, config.webAuthRateLimitMs)) return;
  const String host = req.arg("host");
  const String rawPort = req.arg("port");
  const String certificatePem = req.arg("cert");
  const String privateKeyPem = req.arg("key");
  if (host.isEmpty() || host.length() > 253 || rawPort.isEmpty() ||
      rawPort.length() > 5 || certificatePem.length() < 64 ||
      certificatePem.length() > 8192 || privateKeyPem.length() < 64 ||
      privateKeyPem.length() > 8192) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid MQTT PKI material\"}");
    return;
  }
  uint32_t port = 0;
  for (size_t i = 0; i < rawPort.length(); ++i) {
    if (rawPort[i] < '0' || rawPort[i] > '9') {
      res.sendJson(400, "{\"ok\":false,\"error\":\"invalid MQTT port\"}");
      return;
    }
    port = port * 10U + static_cast<uint32_t>(rawPort[i] - '0');
  }
  if (port == 0 || port > 65535U || host.indexOf('|') >= 0 ||
      certificatePem.indexOf("-----BEGIN CERTIFICATE-----") < 0 ||
      certificatePem.indexOf("-----END CERTIFICATE-----") < 0 ||
      privateKeyPem.indexOf("-----BEGIN") < 0 ||
      privateKeyPem.indexOf("PRIVATE KEY-----") < 0) {
    res.sendJson(400, "{\"ok\":false,\"error\":\"invalid MQTT PKI material\"}");
    return;
  }
  if (!mqtt.provisionCertificate(host, static_cast<uint16_t>(port),
                                 certificatePem, privateKeyPem)) {
    res.sendJson(503, "{\"ok\":false,\"error\":\"MQTT PKI provisioning failed\"}");
    return;
  }
  res.sendJson(200, "{\"ok\":true,\"provisioned\":true,\"auth\":\"x509\"}");
}

void WebUi::handleMqttStatus(HttpdRequest& req, HttpdResponse& res) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  static uint32_t lastMqttStatusMs = 0;
  if (!rateLimit(req, res, lastMqttStatusMs, config.webAuthRateLimitMs)) return;
  String j = "{\"ok\":true,\"provisioned\":";
  j += mqtt.credentialsProvisioned() ? "true" : "false";
  j += ",\"auth\":\"x509\"";
  j += ",\"connected\":";
  j += mqtt.isConnected() ? "true" : "false";
  j += ",\"passwordRotationWarning\":";
  j += mqtt.passwordRotationWarning() ? "true" : "false";
  j += "}";
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, j);
}

void WebUi::handleMqttCertStatus(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, certLifecycle.statusJson());
}

void WebUi::handleMqttCertRenew(HttpdRequest& req, HttpdResponse& res) {
  static uint32_t lastRenewMs = 0;
  if (!rateLimit(req, res, lastRenewMs, 60000UL)) return;
  const bool ok = certLifecycle.renewCertificate(true);
  res.sendJson(ok ? 200 : 503,
               ok ? "{\"ok\":true,\"status\":\"renewed\"}"
                  : "{\"ok\":false,\"status\":\"renew_failed\"}");
}

void WebUi::handleMqttCertHistory(HttpdRequest& /*req*/, HttpdResponse& res) {
  res.setHeader("Cache-Control", "no-store");
  res.sendJson(200, certLifecycle.historyJson());
}

void WebUi::handleMqttCertCaChain(HttpdRequest& req, HttpdResponse& res) {
  static uint32_t lastCaFetchMs = 0;
  if (!rateLimit(req, res, lastCaFetchMs, 60000UL)) return;
  const bool ok = certLifecycle.fetchCaChain();
  res.sendJson(ok ? 200 : 503,
               ok ? "{\"ok\":true,\"status\":\"fetched\"}"
                  : "{\"ok\":false,\"status\":\"fetch_failed\"}");
}

// ---------------------------------------------------------------------------
// Handlers: config / factory reset / reboot
// ---------------------------------------------------------------------------
void WebUi::handleConfigExport(HttpdRequest& /*req*/, HttpdResponse& res) {
  RuntimeConfig c;
  if (!configSnapshot(c)) {
    res.send503("configuration unavailable");
    return;
  }
  String j = "{";
  j += "\"freq\":" + String(c.loraFreqMHz, 3);
  j += ",\"bw\":" + String(c.loraBwKHz, 3);
  j += ",\"sf\":" + String(c.loraSf) + ",\"cr\":" + String(c.loraCr);
  j += ",\"sync\":" + String(c.loraSyncWord) + ",\"power\":" + String(c.loraPowerDbm);
  j += ",\"volume\":" + String(c.volume) + ",\"audio_source\":" + String(c.audioRecordSource);
  j += ",\"record_quality\":" + String(c.audioRecordQuality);
  j += ",\"ble_pairing\":" + String(c.blePairingEnabled ? "true" : "false");
  j += ",\"battery_calibration\":" + String(c.batteryCalibration, 5);
  j += ",\"mqtt_enabled\":" + String(c.mqttEnabled ? "true" : "false");
  j += ",\"mqtt_host\":\"" + jsonEscape(c.mqttHost) + "\"";
  j += ",\"mqtt_port\":" + String(c.mqttPort);
  j += ",\"mqtt_tls\":" + String(c.mqttTlsRequired ? "true" : "false");
  j += ",\"mqtt_reconnect_min_ms\":" + String(c.mqttReconnectMinMs);
  j += ",\"mqtt_reconnect_max_ms\":" + String(c.mqttReconnectMaxMs);
  j += ",\"mqtt_telemetry_period_ms\":" + String(c.mqttTelemetryPeriodMs);
  j += ",\"mqtt_health_period_ms\":" + String(c.mqttHealthPeriodMs);
  j += ",\"mqtt_retain_telemetry\":" + String(c.mqttRetainTelemetry ? "true" : "false");
  j += ",\"mqtt_retain_availability\":" + String(c.mqttRetainAvailability ? "true" : "false");
  j += ",\"mqtt_rotation_days\":" + String(c.mqttCredentialRotationDays);
  j += ",\"vox_enabled\":" + String(c.voxEnabled ? "true" : "false");
  j += ",\"vox_threshold\":" + String(c.voxThreshold, 4);
  j += ",\"vox_hang_ms\":" + String(c.voxHangMs);
  j += ",\"aec_enabled\":" + String(c.aecEnabled ? "true" : "false");
  j += ",\"usb_monitor\":" + String(c.usbMonitor ? "true" : "false");
  j += ",\"usb_transport\":" + String(c.usbPlaybackTransport ? "true" : "false");
  j += ",\"loopback\":" + String(c.audioLoopback ? "true" : "false");
  j += ",\"adr_enabled\":" + String(c.loraAdrEnabled ? "true" : "false");
  j += ",\"hop_enabled\":" + String(c.loraHopEnabled ? "true" : "false");
  j += ",\"hop_profile\":" + String(c.loraHopChannelProfile);
  j += ",\"range_test_mode\":" + String(c.loraRangeTestMode ? "true" : "false");
  j += ",\"ble_enabled\":" + String(c.sensorReaderEnabled ? "true" : "false");
  j += ",\"ble_scan_interval_ms\":" + String(c.sensorScanIntervalMs);
  j += ",\"ble_scan_window_ms\":" + String(c.sensorScanWindowMs);
  j += ",\"ble_scan_duration_ms\":" + String(c.sensorScanDurationMs);
  j += ",\"ble_connect_timeout_ms\":" + String(c.sensorConnectTimeoutMs);
  j += ",\"ble_eviction_ms\":" + String(c.sensorNodeEvictionMs);
  j += ",\"ble_max_nodes\":" + String(c.sensorMaxNodes);
  j += ",\"ble_encryption\":" + String(c.sensorRequireEncryption ? "true" : "false");
  j += ",\"ble_failure_threshold\":" + String(c.blePairingFailureThreshold);
  j += ",\"ble_block_ms\":" + String(c.blePairingBlockMs);
  j += ",\"ble_keep_awake\":" + String(c.sensorKeepAwake ? "true" : "false");
  j += ",\"web_session_timeout_ms\":" + String(c.webSessionTimeoutMs);
  j += ",\"web_auth_rate_limit_ms\":" + String(c.webAuthRateLimitMs);
  j += ",\"csrf_policy\":" + String(c.csrfPolicy);
  j += ",\"ble_pairing_policy\":" + String(c.blePairingPolicy);
  j += ",\"ecdh_rekey_policy\":" + String(c.ecdhRekeyPolicy);
  j += ",\"replay_window_bits\":" + String(c.replayWindowBits);
  j += ",\"est_server_url\":\"" + jsonEscape(c.estServerUrl) + "\"";
  j += ",\"est_label\":\"" + jsonEscape(c.estLabel) + "\"";
  j += ",\"est_auth_mode\":" + String(c.estAuthMode);
  j += ",\"cert_renewal_threshold_days\":" + String(c.certRenewalThresholdDays);
  j += ",\"cert_check_period_ms\":" + String(c.certCheckPeriodMs);
  j += ",\"cert_lifecycle_enabled\":" + String(c.certLifecycleEnabled ? "true" : "false");
  j += ",\"wake_period_sec\":" + String(c.wakePeriodSec);
  j += ",\"deep_sleep_enabled\":" + String(c.deepSleepEnabled ? "true" : "false");
  j += ",\"deep_sleep_idle_sec\":" + String(c.deepSleepIdleMs / 1000UL);
  j += ",\"deep_sleep_wake_grace_ms\":" + String(c.deepSleepWakeGraceMs);
  j += ",\"critical_shutdown_delay_ms\":" + String(c.criticalShutdownDelayMs);
  j += ",\"battery_low_threshold\":" + String(c.batteryLowThreshold, 3);
  j += ",\"battery_critical_threshold\":" + String(c.batteryCriticalThreshold, 3);
  j += ",\"classd_enabled\":" + String(c.classDEnabled ? "true" : "false");
  j += ",\"classd_boost\":" + String(c.classDBoostLevel);
  j += ",\"callsign\":\"" + jsonEscape(c.callsign) + "\"";
  j += "}";
  res.setHeader("Content-Disposition",
                "attachment; filename=\"fieldradio-config.json\"");
  res.sendJson(200, j);
}

void WebUi::handleConfigBackup(HttpdRequest& /*req*/, HttpdResponse& res) {
  const String envelope = encryptConfigBackup();
  if (envelope.isEmpty()) {
    res.send503("backup unavailable");
    return;
  }
  res.setHeader("Content-Disposition",
                "attachment; filename=\"fieldradio-config.frb\"");
  res.sendText(200, envelope);
}

void WebUi::handleConfigRestore(HttpdRequest& req, HttpdResponse& res) {
  String body;
  if (!req.bodyText(body, 4096)) {
    res.sendText(413, "backup too large"); return;
  }
  if (body.length() < 16) {
    res.sendText(400, "invalid backup");
    return;
  }
  String plain;
  if (!decryptConfigBackup(body, plain)) {
    res.sendText(400, "backup authentication failed");
    return;
  }

  RuntimeConfig candidate{};
  if (!configSnapshot(candidate)) { res.send503("configuration snapshot unavailable"); return; }
  bool seenFreq = false, seenBw = false, seenSf = false, seenCr = false;
  int pos = 0;
  while (pos <= static_cast<int>(plain.length())) {
    const int nl = plain.indexOf('\n', pos);
    const int end = nl < 0 ? plain.length() : nl;
    const String line = plain.substring(pos, end);
    String key, value;
    if (!line.isEmpty()) {
      if (!parseBackupLine(line, key, value)) {
        res.sendText(400, "malformed backup");
        return;
      }
      if (key == "freq") { candidate.loraFreqMHz = value.toFloat(); seenFreq = true; }
      else if (key == "bw") { candidate.loraBwKHz = value.toFloat(); seenBw = true; }
      else if (key == "sf") { candidate.loraSf = static_cast<uint8_t>(value.toInt()); seenSf = true; }
      else if (key == "cr") { candidate.loraCr = static_cast<uint8_t>(value.toInt()); seenCr = true; }
      else if (key == "sync") candidate.loraSyncWord = static_cast<uint8_t>(value.toInt());
      else if (key == "power") candidate.loraPowerDbm = static_cast<int8_t>(value.toInt());
      else if (key == "volume") candidate.volume = static_cast<uint8_t>(value.toInt());
      else if (key == "audsrc") candidate.audioRecordSource = static_cast<uint8_t>(value.toInt());
      else if (key == "recqual") candidate.audioRecordQuality = static_cast<uint8_t>(value.toInt());
      else if (key == "batcal") candidate.batteryCalibration = value.toFloat();
      else if (key == "callsign") candidate.callsign = value;
      else if (key == "lorakey") candidate.loraKeyHex = value;
      else if (key == "apssid") candidate.apSsid = value;
      else if (key == "apppass") candidate.apPassword = value;
      else if (key == "stassid") candidate.staSsid = value;
      else if (key == "stapass") candidate.staPassword = value;
      else if (key == "webuser") candidate.webUser = value;
      else if (key == "websalt") candidate.webPasswordSaltHex = value;
      else if (key == "webph") candidate.webPasswordHashHex = value;
      else if (key == "mqtt_en") candidate.mqttEnabled = value == "1";
      else if (key == "wake_sec") candidate.wakePeriodSec = static_cast<uint32_t>(value.toInt());
      else if (key == "sleep_en") candidate.deepSleepEnabled = value == "1";
      else if (key == "sleep_idle") candidate.deepSleepIdleMs = static_cast<uint32_t>(value.toInt());
      else if (key == "wake_grace") candidate.deepSleepWakeGraceMs = static_cast<uint32_t>(value.toInt());
      else if (key == "bat_crit_delay") candidate.criticalShutdownDelayMs = static_cast<uint32_t>(value.toInt());
      else if (key == "bat_low") candidate.batteryLowThreshold = value.toFloat();
      else if (key == "bat_critical") candidate.batteryCriticalThreshold = value.toFloat();
      else if (key == "classd_en") candidate.classDEnabled = value == "1";
      else if (key == "classd_boost") candidate.classDBoostLevel = static_cast<uint8_t>(value.toInt());
      else { res.sendText(400, "unknown backup key"); return; }
    }
    if (nl < 0) break;
    pos = nl + 1;
  }
  if (!seenFreq || !seenBw || !seenSf || !seenCr || !candidate.validSemantics() ||
      candidate.volume > 100 || candidate.audioRecordQuality > 2 ||
      candidate.audioRecordSource > Config::AUDIO_SOURCE_USB ||
      candidate.wakePeriodSec < Config::WAKE_PERIOD_SEC_MIN || candidate.wakePeriodSec > Config::WAKE_PERIOD_SEC_MAX ||
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
      !candidate.webPasswordConfigured()) {
    res.sendText(400, "backup config invalid");
    return;
  }

  RuntimeConfig previous{};
  if (!configSnapshot(previous)) { res.send503("configuration snapshot unavailable"); return; }
  const uint8_t previousSource = audio.recordSource();
  if (!configCommit(candidate)) { res.send503("NVS restore failed"); return; }
  auto rollback = [&]() {
    (void)configCommit(previous); (void)lora.applyConfig();
    (void)audio.setClassDConfig(previous.classDEnabled,previous.classDBoostLevel);
    (void)audio.setVox(previous.voxEnabled,previous.voxThreshold,previous.voxHangMs);
    (void)audio.setAec(previous.aecEnabled); (void)audio.setUsbMonitor(previous.usbMonitor);
    (void)audio.setUsbPlaybackTransport(previous.usbPlaybackTransport); (void)audio.setLoopback(previous.audioLoopback);
    (void)audio.applyRecordQualityRuntime(previous.audioRecordQuality); (void)audio.setRecordSource(previousSource);
    mqtt.setEnabled(previous.mqttEnabled); audio.setVolume(previous.volume);
  };
  if (!audio.setClassDConfig(candidate.classDEnabled,candidate.classDBoostLevel) ||
      !audio.setVox(candidate.voxEnabled,candidate.voxThreshold,candidate.voxHangMs) ||
      !audio.setAec(candidate.aecEnabled) || !audio.setUsbMonitor(candidate.usbMonitor) ||
      !audio.setUsbPlaybackTransport(candidate.usbPlaybackTransport) || !audio.setLoopback(candidate.audioLoopback) ||
      !audio.applyRecordQualityRuntime(candidate.audioRecordQuality)) {
    rollback(); res.send503("audio restore failed"); return;
  }
  mqtt.setEnabled(candidate.mqttEnabled);
  if (!lora.applyConfig()) { rollback(); res.send503("radio restore failed"); return; }
  if (!audio.setRecordSource(candidate.audioRecordSource)) { rollback(); res.send503("audio source restore failed"); return; }
  audio.setVolume(candidate.volume);

  lora.updateSourceId();
  res.sendText(200, "OK; reboot recommended");
}

void WebUi::handleFactoryReset(HttpdRequest& req, HttpdResponse& res) {
  if (req.arg("confirm") != "RESET") {
    res.sendText(400, "confirmation required");
    return;
  }
  if (!lora.prepareForFactoryReset()) {
    res.send503("LoRa storage busy");
    return;
  }
  {
    SpiLock spiLock(pdMS_TO_TICKS(500));
    if (!spiLock.ok()) {
      res.send503("storage busy");
      return;
    }
    static const char* const managedDirs[] = {"/REC", "/LOG", "/TRACK", "/LORA"};
    for (const char* dir : managedDirs) {
      if (SD.exists(dir) && !eraseStorageTree(dir)) {
        lora.cancelFactoryReset();
        res.send503("SD data erase failed");
        return;
      }
    }
  }

  const esp_err_t err = nvs_flash_erase();
  if (err != ESP_OK) {
    lora.cancelFactoryReset();
    res.send503("NVS secure erase failed");
    return;
  }
  res.sendText(200, "factory reset; rebooting");
  delay(100);
  ESP.restart();
}

void WebUi::handleReboot(HttpdRequest& /*req*/, HttpdResponse& res) {
  (void)sensorSpool.flush();
  res.sendText(200, "rebooting");
  delay(100);
  ESP.restart();
}

// === END OF WebUi.cpp ===
```

---

Status Setelah Pesan 6c

WebUi.cpp selesai — berisi 121 handler + semua helper + begin() + task().

File Status
HttpdServer.h/.cpp ✅ Pesan 1
HttpdRequest.h/.cpp ✅ Pesan 2
HttpdResponse.h/.cpp ✅ Pesan 2
HttpdMultipart.h/.cpp ✅ Pesan 3
WebUi.h ✅ Pesan 4
WebUi.cpp Bagian 1+2 ✅ Pesan 5
WebUi.cpp Bagian 3a+3b+3c ✅ Pesan 6
main.cpp diff ⏳ Pesan 7
platformio.ini diff ⏳ Pesan 7

Catatan Penting: handleConfig Belum Ada!

Saya baru sadar: WebUi.h deklarasikan handleConfig(HttpdRequest&, HttpdResponse&) dan begin() meregister /api/config HTTP_POST → handleConfig. Tapi saya belum menulis implementasinya di 6a/6b/6c. Ini handler paling kompleks (~50 argumen body urlencoded, config transaction, rollback).

Tunggu — cek ulang. Di daftar 6a saya tulis sampai handleMessagePersist lalu langsung ke handleMessageSchedule. Handler handleConfig tidak masuk di manapun. Saya lupa.

Solusi: Anda build sekarang dengan error undefined reference to WebUi::handleConfig. Saya akan kirim handleConfig sebagai Pesan 6d (suplemen) bersama Pesan 7.

Kalau saya paksa selipkan sekarang, pesan ini jadi terlalu panjang dan berisiko terpotong. Saya kirim Pesan 6d terpisah, lalu Pesan 7 (main.cpp + platformio.ini).

Konfirmasi kalau Pesan 6c sudah tersalin, saya lanjut ke Pesan 6d: handleConfig() — handler terakhir yang hilang.