Iterasi C — Pesan 5b dari 7: WebUi.cpp Bagian 2

Sambung ke File yang Sama

Blok di bawah ini disambung langsung setelah )HTML"; di WebUi.cpp. Jangan ada #include tambahan — semua sudah di Bagian 1.

---

```cpp
// ---------------------------------------------------------------------------
// Free helpers (lanjutan Bagian 1)
// ---------------------------------------------------------------------------

static String configBackupPlaintext() {
  RuntimeConfig c;
  if (!configSnapshot(c)) return String();
  String p;
  p.reserve(512);
  p += "freq=" + String(c.loraFreqMHz, 6) + "\n";
  p += "bw=" + String(c.loraBwKHz, 6) + "\n";
  p += "sf=" + String(c.loraSf) + "\n";
  p += "cr=" + String(c.loraCr) + "\n";
  p += "sync=" + String(c.loraSyncWord) + "\n";
  p += "power=" + String(c.loraPowerDbm) + "\n";
  p += "volume=" + String(c.volume) + "\n";
  p += "audsrc=" + String(c.audioRecordSource) + "\n";
  p += "recqual=" + String(c.audioRecordQuality) + "\n";
  p += "batcal=" + String(c.batteryCalibration, 6) + "\n";
  p += "callsign=" + c.callsign + "\n";
  p += "lorakey=" + c.loraKeyHex + "\n";
  p += "apssid=" + c.apSsid + "\n";
  p += "apppass=" + c.apPassword + "\n";
  p += "stassid=" + c.staSsid + "\n";
  p += "stapass=" + c.staPassword + "\n";
  p += "webuser=" + c.webUser + "\n";
  p += "websalt=" + c.webPasswordSaltHex + "\n";
  p += "webph=" + c.webPasswordHashHex + "\n";
  p += "mqtt_en=" + String(c.mqttEnabled ? 1 : 0) + "\n";
  p += "wake_sec=" + String(c.wakePeriodSec) + "\n";
  p += "sleep_en=" + String(c.deepSleepEnabled ? 1 : 0) + "\n";
  p += "sleep_idle=" + String(c.deepSleepIdleMs) + "\n";
  p += "wake_grace=" + String(c.deepSleepWakeGraceMs) + "\n";
  p += "bat_crit_delay=" + String(c.criticalShutdownDelayMs) + "\n";
  p += "bat_low=" + String(c.batteryLowThreshold, 3) + "\n";
  p += "bat_critical=" + String(c.batteryCriticalThreshold, 3) + "\n";
  p += "classd_en=" + String(c.classDEnabled ? 1 : 0) + "\n";
  p += "classd_boost=" + String(c.classDBoostLevel) + "\n";
  return p;
}

static bool backupKey(uint8_t key[32]) {
  RuntimeConfig config;
  if (!key || !configSnapshot(config) || config.webPasswordHashHex.length() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const char a = config.webPasswordHashHex[i * 2];
    const char b = config.webPasswordHashHex[i * 2 + 1];
    auto n = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = n(a), lo = n(b);
    if (hi < 0 || lo < 0) return false;
    key[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

static String encryptConfigBackup() {
  uint8_t key[32] = {};
  if (!backupKey(key)) return String();
  uint8_t iv[16] = {};
  for (size_t i = 0; i < sizeof(iv); i += 4) {
    const uint32_t r = esp_random();
    memcpy(iv + i, &r, min<size_t>(4, sizeof(iv) - i));
  }
  const String plain = configBackupPlaintext();
  String cipherHex;
  cipherHex.reserve(plain.length() * 2);
  uint8_t* cipher = static_cast<uint8_t*>(malloc(plain.length()));
  if (!cipher) return String();
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t ncOff = 0;
  uint8_t stream[16] = {};
  uint8_t ctr[16] = {};
  memcpy(ctr, iv, sizeof(iv));
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
                  mbedtls_aes_crypt_ctr(&aes, plain.length(), &ncOff, ctr, stream,
                                        reinterpret_cast<const unsigned char*>(plain.c_str()), cipher) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) { free(cipher); return String(); }
  cipherHex = hexEncodeUi(cipher, plain.length());
  free(cipher);

  String envelope = "FRB1|" + hexEncodeUi(iv, sizeof(iv)) + "|" + cipherHex;
  uint8_t mac[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
                             reinterpret_cast<const uint8_t*>(envelope.c_str()),
                             envelope.length(), mac) != 0)
    return String();
  return envelope + "|" + hexEncodeUi(mac, sizeof(mac));
}

static bool decryptConfigBackup(const String& envelope, String& plain) {
  uint8_t key[32] = {};
  if (!backupKey(key)) return false;
  const int p1 = envelope.indexOf('|');
  const int p2 = p1 >= 0 ? envelope.indexOf('|', p1 + 1) : -1;
  const int p3 = p2 >= 0 ? envelope.indexOf('|', p2 + 1) : -1;
  if (p1 != 4 || p2 <= p1 || p3 <= p2) return false;
  const String tag = envelope.substring(p3 + 1);
  uint8_t iv[16] = {}, mac[32] = {};
  if (!hexDecodeUi(envelope.substring(p1 + 1, p2), iv, sizeof(iv)) ||
      !hexDecodeUi(tag, mac, sizeof(mac)))
    return false;
  const String signedPart = envelope.substring(0, p3);
  uint8_t expected[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
                             reinterpret_cast<const uint8_t*>(signedPart.c_str()),
                             signedPart.length(), expected) != 0)
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(mac); ++i) diff |= mac[i] ^ expected[i];
  if (diff) return false;
  const String cipherHex = envelope.substring(p2 + 1, p3);
  if (cipherHex.length() == 0 || (cipherHex.length() & 1U)) return false;
  const size_t len = cipherHex.length() / 2;
  uint8_t* cipher = static_cast<uint8_t*>(malloc(len));
  uint8_t* out = static_cast<uint8_t*>(malloc(len + 1));
  if (!cipher || !out || !hexDecodeUi(cipherHex, cipher, len)) {
    free(cipher); free(out); return false;
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t ncOff = 0;
  uint8_t stream[16] = {}, ctr[16] = {};
  memcpy(ctr, iv, sizeof(iv));
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
                  mbedtls_aes_crypt_ctr(&aes, len, &ncOff, ctr, stream,
                                        cipher, out) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) { free(cipher); free(out); return false; }
  out[len] = '\0';
  plain = String(reinterpret_cast<char*>(out));
  free(cipher); free(out);
  return true;
}

static bool parseBackupLine(const String& line, String& key, String& value) {
  const int eq = line.indexOf('=');
  if (eq <= 0) return false;
  key = line.substring(0, eq);
  value = line.substring(eq + 1);
  return true;
}

static bool isValidUploadedWav(const String& path) {
  if (path.length() < 4 ||
      !path.substring(path.length() - 4).equalsIgnoreCase(".WAV"))
    return false;
  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) return false;
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory() || f.size() < 44) {
    if (f) f.close();
    return false;
  }
  uint8_t header[12] = {};
  const size_t got = f.read(header, sizeof(header));
  f.close();
  return got == sizeof(header) &&
         memcmp(header, "RIFF", 4) == 0 &&
         memcmp(header + 8, "WAVE", 4) == 0;
}

static bool eraseStorageTree(const char* path) {
  if (!path || !*path) return false;
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  bool ok = true;
  for (;;) {
    File entry = dir.openNextFile();
    if (!entry) break;
    String child = entry.name();
    const bool isDir = entry.isDirectory();
    entry.close();
    if (!child.startsWith("/")) {
      child = String(path) + "/" + child;
    }

    const bool childOk = isDir
        ? eraseStorageTree(child.c_str())
        : SD.remove(child);
    if (!childOk) ok = false;
  }
  dir.close();
  return ok;
}

// ---------------------------------------------------------------------------
// External dependencies (unchanged from pre-migration WebUi.cpp)
// ---------------------------------------------------------------------------
extern StorageManager storage;
extern LoRaManager lora;
extern LoRaWANManager lorawan;
extern AudioManager audio;
extern MqttClientManager mqtt;
extern BleSensorReader bleSensorReader;
extern void fieldRadioRequestDeepSleep();
static SensorReader::SensorNodeSnapshot gSensorSnapshots[SensorRegistry::MAX_SUPPORTED_NODES]{};

// ---------------------------------------------------------------------------
// Sensor JSON helpers (unchanged from pre-migration WebUi.cpp)
// ---------------------------------------------------------------------------
static String sensorAddressJson(const SensorProtocol::BleAddress& address) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(18);
  for (int i = 5; i >= 0; --i) {
    if (i != 5) out += ':';
    out += hex[address.bytes[i] >> 4];
    out += hex[address.bytes[i] & 0x0F];
  }
  return out;
}

static String sensorNodeJson(size_t index, const SensorRegistry::Node& node, bool includeValues) {
  String j = "{\"id\":" + String(index) + ",\"address\":\"" + sensorAddressJson(node.address) +
             "\",\"name\":\"" + jsonEscape(String(node.name)) + "\",\"rssi\":" + String(node.rssi) +
             ",\"connected\":" + String(node.connected ? "true" : "false") +
             ",\"lastSeenMs\":" + String(node.lastSeenMs) + ",\"sensorCount\":" + String(node.sensorCount);
  if (node.hasRPA) j += ",\"rpa\":\"" + sensorAddressJson(node.lastRPA) + "\"";
  if (includeValues) {
    j += ",\"sensors\":[";
    for (size_t i = 0; i < node.sensorCount; ++i) {
      if (i) j += ',';
      const auto& d = node.descriptors[i];
      const auto& v = node.values[i];
      j += "{\"id\":" + String(d.id) + ",\"name\":\"" + jsonEscape(String(d.name)) +
           "\",\"unit\":\"" + jsonEscape(String(d.unit)) + "\",\"valueValid\":" +
           String(node.valueValid[i] ? "true" : "false") + ",\"value\":" +
           (node.valueValid[i] ? String(v.value, 6) : String("null")) +
           ",\"timestamp\":" + (node.valueValid[i] ? String(v.timestamp) : String("null")) +
           ",\"quality\":" + String(node.valueValid[i] ? v.quality : 0) + "}";
    }
    j += ']';
  }
  return j + '}';
}

// ---------------------------------------------------------------------------
// Config audit (unchanged)
// ---------------------------------------------------------------------------
static void auditConfigChange(const RuntimeConfig& previous,
                              const RuntimeConfig& current,
                              const char* reason) {
  if (!storage.ready()) return;
  SpiLock spiLock(pdMS_TO_TICKS(50));
  if (!spiLock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  const char* path = "/LOG/CONFIG-AUDIT.LOG";
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (f.size() >= Config::CONFIG_AUDIT_LOG_ROTATE_BYTES) {
    f.close();
    const char* old = "/LOG/CONFIG-AUDIT.1.LOG";
    if (SD.exists(old)) SD.remove(old);
    if (SD.exists(path)) SD.rename(path, old);
    f = SD.open(path, FILE_APPEND);
  }
  if (!f) return;
  StateLock lock(gState);
  const uint64_t ts = lock.ok() && gState.gps.timeValid ? gState.gps.utcEpoch : millis();
  f.printf("%llu,%s,freq=%.3f,bw=%.1f,sf=%u,pwr=%d,volume=%u,audio=%u,reason=%s\n",
           static_cast<unsigned long long>(ts), current.callsign.c_str(),
           current.loraFreqMHz, current.loraBwKHz,
           current.loraSf, current.loraPowerDbm, current.volume,
           current.audioRecordSource, reason ? reason : "web");
  f.close();
}

// ---------------------------------------------------------------------------
// begin() — register all endpoints and start TLS server
// ---------------------------------------------------------------------------
void WebUi::begin() {
#if !WEB_TLS_CERT_CONFIGURED
  // Never fall back to plaintext HTTP when certificate provisioning is absent.
  // The WebUI remains disabled until a device-specific certificate/key pair is
  // provisioned through secrets/.
  Serial.println("WebUI HTTPS disabled: TLS certificate provisioning missing");
  return;
#endif

  // Every endpoint runs through the native HttpdServer. auth() and csrfValid()
  // are per-request; the lambdas capture `this` so no per-endpoint state has
  // to be stored in the handler table.
  auto reg = [this](const char* uri, httpd_method_t method,
                    std::function<void(HttpdRequest&, HttpdResponse&)> fn) {
    if (!server_.on(uri, method, std::move(fn))) {
      Serial.printf("WebUI: failed to register %s\n", uri);
    }
  };

  // ---- Root & meta ----
  reg("/", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRoot(req, res);
  });
  reg("/api/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStatus(req, res);
  });
  reg("/api/v1/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStatus(req, res);
  });
  reg("/api/version", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleApiVersion(req, res);
  });
  reg("/api/v1/version", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleApiVersion(req, res);
  });
  reg("/api/v1/csrf", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    res.sendJson(200, "{\"token\":\"" + csrfTokenHexForActiveSession() + "\"}");
  });

  // ---- LoRaWAN ----
  reg("/api/lorawan/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANStatus(req, res);
  });
  reg("/api/lorawan/connect", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANConnect(req, res);
  });
  reg("/api/lorawan/disconnect", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANDisconnect(req, res);
  });
  reg("/api/lorawan/config", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANConfig(req, res);
  });
  reg("/api/lorawan/uplink", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoRaWANUplink(req, res);
  });

  // ---- Files ----
  reg("/api/files", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleFiles(req, res);
  });
  reg("/api/download", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDownload(req, res);
  });
  reg("/api/upload", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleUpload(req, res);
  });
  reg("/api/rename", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRename(req, res);
  });

  // ---- Messages ----
  reg("/api/messages", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessages(req, res);
  });
  reg("/api/messages/clear", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageClear(req, res);
  });
  reg("/api/messages/read", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageRead(req, res);
  });
  reg("/api/messages/reply", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageReply(req, res);
  });
  reg("/api/messages/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageExport(req, res);
  });
  reg("/api/messages/persist", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessagePersist(req, res);
  });
  reg("/api/message/schedule", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageSchedule(req, res);
  });
  reg("/api/message/schedule", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageScheduleList(req, res);
  });
  reg("/api/message/schedule", HTTP_DELETE, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMessageScheduleDelete(req, res);
  });
  reg("/api/record/schedule", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordSchedule(req, res);
  });
  reg("/api/record/schedule", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordScheduleGet(req, res);
  });

  // ---- SOS ----
  reg("/api/sos/format", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSosFormat(req, res);
  });
  reg("/api/sos-history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSosHistory(req, res);
  });
  reg("/api/sos", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSos(req, res);
  });
  reg("/api/sos-status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSosStatus(req, res);
  });

  // ---- Self test ----
  reg("/api/selftest", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSelfTest(req, res);
  });
  reg("/api/selftest/result", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSelfTestResult(req, res);
  });

  // ---- UI prefs ----
  reg("/api/lang", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLang(req, res);
  });
  reg("/api/theme", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTheme(req, res);
  });

  // ---- Diagnostics ----
  reg("/api/neighbors", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleNeighbors(req, res);
  });
  reg("/api/routes", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRoutes(req, res);
  });
  reg("/api/dedup/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDedupStats(req, res);
  });
  reg("/api/forward/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleForwardStats(req, res);
  });
  reg("/api/auth/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleAuthStats(req, res);
  });
  reg("/api/nvs", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleNvs(req, res);
  });
  reg("/api/config/migrate", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigMigrate(req, res);
  });
  reg("/api/diag/full", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDiagFull(req, res);
  });
  reg("/api/health-log", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHealthLog(req, res);
  });
  reg("/api/lora-log", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLoraLog(req, res);
  });

  // ---- Capture / ADR / HOP ----
  reg("/api/capture/start", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleCaptureStart(req, res);
  });
  reg("/api/capture/stop", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleCaptureStop(req, res);
  });
  reg("/api/capture/dump", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleCaptureDump(req, res);
  });
  reg("/api/adr", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleAdr(req, res);
  });
  reg("/api/hop/sync", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopSync(req, res);
  });
  reg("/api/hop/suggest", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopSuggest(req, res);
  });
  reg("/api/hop/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopStatus(req, res);
  });
  reg("/api/hop/enable", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopEnable(req, res);
  });
  reg("/api/hop/set-channels", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleHopSetChannels(req, res);
  });

  // ---- Scanner / range test ----
  reg("/api/scan/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanStatus(req, res);
  });
  reg("/api/scan/start", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanStart(req, res);
  });
  reg("/api/scan/stop", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanStop(req, res);
  });
  reg("/api/scan/results", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleScanResults(req, res);
  });
  reg("/api/range-test", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRangeTest(req, res);
  });
  reg("/api/range-test/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRangeTestStatus(req, res);
  });
  reg("/api/range-test/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRangeTestStatus(req, res);
  });

  // ---- Radio / battery / storage ----
  reg("/api/radio/history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRadioHistory(req, res);
  });
  reg("/api/radio/tune", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRadioTune(req, res);
  });
  reg("/api/radio/stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRadioStats(req, res);
  });
  reg("/api/rf/detector", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRfDetector(req, res);
  });
  reg("/api/battery/history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBatteryHistory(req, res);
  });
  reg("/api/battery-calibrate", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBatteryCalibrate(req, res);
  });
  reg("/api/storage/info", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStorageInfo(req, res);
  });
  reg("/api/storage/checksum", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleChecksum(req, res);
  });
  reg("/api/storage/checksum-sha256", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleChecksumSha256(req, res);
  });
  reg("/api/log/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleLogExport(req, res);
  });

  // ---- Audio ----
  reg("/api/ptt", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handlePtt(req, res);
  });
  reg("/api/record", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecord(req, res);
  });
  reg("/api/record/quality", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordQuality(req, res);
  });
  reg("/api/play", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handlePlay(req, res);
  });
  reg("/api/stop", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleStop(req, res);
  });
  reg("/api/pause", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handlePause(req, res);
  });
  reg("/api/seek", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSeek(req, res);
  });
  reg("/api/queue", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleQueue(req, res);
  });
  reg("/api/queue-clear", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleQueueClear(req, res);
  });
  reg("/api/record-pause", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordPause(req, res);
  });
  reg("/api/record-split", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleRecordSplit(req, res);
  });
  reg("/api/vox", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleVox(req, res);
  });
  reg("/api/vad", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleVad(req, res);
  });
  reg("/api/usb-transport", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleUsbTransport(req, res);
  });
  reg("/api/volume", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleVolume(req, res);
  });
  reg("/api/delete", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleDelete(req, res);
  });
  reg("/api/audio-source", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleAudioSource(req, res);
  });
  reg("/api/audio-monitor", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String raw = req.arg("on");
    if (raw != "0" && raw != "1") { res.sendText(400, "invalid monitor"); return; }
    const bool ok = audio.setUsbMonitor(raw == "1");
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });
  reg("/api/audio-loopback", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String raw = req.arg("on");
    if (raw != "0" && raw != "1") { res.sendText(400, "invalid loopback"); return; }
    const bool ok = audio.setLoopback(raw == "1");
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });
  reg("/api/audio-aec", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String raw = req.arg("on");
    if (raw != "0" && raw != "1") { res.sendText(400, "invalid aec"); return; }
    const bool ok = audio.setAec(raw == "1");
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });
  reg("/api/audio-tone", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    const String rf = req.arg("freq");
    const String rm = req.arg("ms");
    if (rf.isEmpty() || rm.isEmpty() || rf.length() > 5 || rm.length() > 4) {
      res.sendText(400, "invalid tone"); return;
    }
    const int f = rf.toInt();
    const int ms = rm.toInt();
    const bool ok = f >= 1 && f <= 10000 && ms >= 1 &&
                    ms <= static_cast<int>(Config::AUDIO_TONE_MAX_MS) &&
                    audio.playTone(static_cast<uint16_t>(f), static_cast<uint16_t>(ms));
    res.sendText(ok ? 200 : 503, ok ? "OK" : "FAIL");
  });

  // ---- Track / GPS ----
  reg("/api/track", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrack(req, res);
  });
  reg("/api/track/points", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrackPoints(req, res);
  });
  reg("/api/track/simplified", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrackSimplified(req, res);
  });
  reg("/api/track/download", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleTrackDownload(req, res);
  });

  // ---- BLE sensors ----
  reg("/api/sensors/nodes", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (!auth(req, res)) return;
    if (req.hasArg("id")) handleSensorNodeDetail(req, res);
    else handleSensorNodes(req, res);
  });
  reg("/api/sensors/live", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorLive(req, res);
  });
  reg("/api/sensors/forget", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorForget(req, res);
  });
  reg("/api/sensors/refresh", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorRefresh(req, res);
  });
  reg("/api/sensors/queue-policy", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorQueuePolicy(req, res);
  });
  reg("/api/sensors/dedup-stats", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorDedupStats(req, res);
  });
  reg("/api/sensors/spool", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorSpool(req, res);
  });
  reg("/api/sensors/spool/clear", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleSensorSpoolClear(req, res);
  });
  reg("/api/ble/passkey", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBlePasskeySet(req, res);
  });
  reg("/api/ble/passkey", HTTP_DELETE, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBlePasskeyDelete(req, res);
  });
  reg("/api/ble/passkey", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleBlePasskeyList(req, res);
  });

  // ---- MQTT ----
  reg("/api/mqtt/provision", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttProvision(req, res);
  });
  reg("/api/mqtt/status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttStatus(req, res);
  });
  reg("/api/mqtt/cert-status", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertStatus(req, res);
  });
  reg("/api/mqtt/cert-renew", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertRenew(req, res);
  });
  reg("/api/mqtt/cert-history", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertHistory(req, res);
  });
  reg("/api/mqtt/cert-cacerts", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleMqttCertCaChain(req, res);
  });

  // ---- Config ----
  reg("/api/config", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfig(req, res);
  });
  reg("/api/config/export", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigExport(req, res);
  });
  reg("/api/config/backup", HTTP_GET, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigBackup(req, res);
  });
  reg("/api/config/restore", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleConfigRestore(req, res);
  });
  reg("/api/factory-reset", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleFactoryReset(req, res);
  });

  // ---- Reboot / deep-sleep ----
  reg("/api/reboot", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) handleReboot(req, res);
  });
  reg("/api/deep-sleep", HTTP_POST, [this](HttpdRequest& req, HttpdResponse& res) {
    if (auth(req, res)) fieldRadioRequestDeepSleep();
  });

  // ---- Start TLS server ----
  if (!server_.begin(WEB_TLS_CERT_DER, WEB_TLS_CERT_DER_LEN,
                     WEB_TLS_KEY_DER, WEB_TLS_KEY_DER_LEN,
                     Config::WEB_PORT)) {
    Serial.println("WebUI: httpd_ssl_start failed; HTTPS service disabled");
    return;
  }
  Serial.printf("WebUI: HTTPS server ready on port %u (%u handlers)\n",
                static_cast<unsigned>(Config::WEB_PORT),
                static_cast<unsigned>(server_.registeredCount()));
}

// ---------------------------------------------------------------------------
// task()
// ---------------------------------------------------------------------------
// The native httpd server owns its own FreeRTOS task. WebUi::task() therefore
// only services the in-process record schedule state machine; it no longer
// drives HTTP request processing.
void WebUi::task() {
  if (recordScheduleActive_ && recordScheduleStart_) {
    bool recording = false;
    uint64_t now = 0;
    {
      StateLock lock(gState);
      if (lock.ok() && gState.gps.timeValid) {
        now = gState.gps.utcEpoch;
        recording = gState.recording;
      }
    }
    if (now >= recordScheduleStart_) {
      if (!recording) (void)audio.startRecording();
      if (now >= recordScheduleStart_ + recordScheduleDurationSec_) {
        (void)audio.stopRecording();
        recordScheduleActive_ = false;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Auth / session / CSRF helpers
// ---------------------------------------------------------------------------

bool WebUi::sameOrigin(HttpdRequest& req) {
  const String origin = req.header("Origin");
  if (origin.isEmpty() || !origin.startsWith("https://")) return false;

  const String host = req.header("Host");
  const String apOrigin = String("https://") + WiFi.softAPIP().toString();
  if (origin == apOrigin) return true;

  if (!host.isEmpty()) {
    String expected = String("https://") + host;
    if (expected.endsWith(":443"))
      expected.remove(expected.length() - 4);
    return origin == expected;
  }
  return false;
}

bool WebUi::rateLimit(HttpdRequest& req, HttpdResponse& res,
                      uint32_t& last, uint32_t interval) {
  struct RateSlot {
    uintptr_t endpoint = 0;
    String ip;
    uint32_t last = 0;
  };
  static RateSlot slots[32];
  const uint32_t now = millis();
  const String ip = req.remoteIp().toString();
  const uintptr_t endpoint = reinterpret_cast<uintptr_t>(&last);

  RateSlot* slot = nullptr;
  RateSlot* oldest = &slots[0];
  for (auto& candidate : slots) {
    if (candidate.endpoint == endpoint && candidate.ip == ip) {
      slot = &candidate;
      break;
    }
    if (candidate.last < oldest->last) oldest = &candidate;
  }
  if (!slot) {
    slot = oldest;
    slot->endpoint = endpoint;
    slot->ip = ip;
    slot->last = 0;
  }
  if (slot->last != 0 && now - slot->last < interval) {
    res.send429("rate limited");
    return false;
  }
  slot->last = now;
  last = now;
  return true;
}

int WebUi::findSessionSlot(const uint8_t token[32], uint32_t clientIp) const {
  if (!token) return -1;
  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    const SessionSlot& slot = sessions_[i];
    if (!slot.inUse || slot.clientIp != clientIp) continue;
    uint8_t msg[8] = {};
    WebUiSessionPolicy::makeSessionMessage(clientIp, slot.issuedMs, msg);
    uint8_t expected[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, slot.secret, sizeof(slot.secret),
                               msg, sizeof(msg), expected, sizeof(expected)) != 0)
      continue;
    uint8_t diff = 0;
    for (size_t j = 0; j < sizeof(expected); ++j)
      diff |= static_cast<uint8_t>(token[j] ^ expected[j]);
    if (diff == 0) return static_cast<int>(i);
  }
  return -1;
}

bool WebUi::sessionValid(HttpdRequest& req) {
  activeSessionSlot_ = -1;
  const String cookie = req.header("Cookie");
  const String prefix = "FR-SESSION=";
  const int start = cookie.indexOf(prefix);
  if (start < 0) return false;
  const int end = cookie.indexOf(';', start);
  const String token = cookie.substring(start + prefix.length(),
                                        end < 0 ? cookie.length() : end);
  if (!WebUiSessionPolicy::isHexToken(
          token.c_str(), token.length(), WebUiSessionPolicy::SESSION_TOKEN_HEX_LENGTH))
    return false;

  uint8_t raw[32] = {};
  for (size_t i = 0; i < sizeof(raw); ++i) {
    const char a = token[i * 2], b = token[i * 2 + 1];
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = hex(a), lo = hex(b);
    if (hi < 0 || lo < 0) return false;
    raw[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  RuntimeConfig config;
  if (!configSnapshot(config))
    return false;
  const uint32_t now = millis();
  const IPAddress ip = req.remoteIp();
  const uint32_t ipValue = static_cast<uint32_t>(ip[0]) |
                           (static_cast<uint32_t>(ip[1]) << 8) |
                           (static_cast<uint32_t>(ip[2]) << 16) |
                           (static_cast<uint32_t>(ip[3]) << 24);

  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    if (sessions_[i].inUse &&
        !WebUiSessionPolicy::isFresh(now, sessions_[i].issuedMs,
                                     config.webSessionTimeoutMs)) {
      sessions_[i].inUse = false;
    }
  }

  const int slot = findSessionSlot(raw, ipValue);
  if (slot < 0) return false;
  if (static_cast<int32_t>(now - authBlockedUntilMs_) < 0) return false;
  if (!WebUiSessionPolicy::isFresh(now, sessions_[slot].issuedMs,
                                   config.webSessionTimeoutMs)) {
    sessions_[slot].inUse = false;
    return false;
  }
  activeSessionSlot_ = static_cast<int8_t>(slot);
  return true;
}

bool WebUi::csrfValid(HttpdRequest& req) {
  if (req.method() != HTTP_POST && req.method() != HTTP_DELETE) return false;
  if (activeSessionSlot_ < 0 ||
      static_cast<size_t>(activeSessionSlot_) >= MAX_SESSIONS) return false;
  const SessionSlot& slot = sessions_[activeSessionSlot_];
  const String supplied = req.header("X-CSRF-Token");
  if (!WebUiSessionPolicy::isHexToken(
          supplied.c_str(), supplied.length(),
          WebUiSessionPolicy::CSRF_TOKEN_HEX_LENGTH)) {
    ++csrfFailures_;
    return false;
  }

  uint8_t diff = 0;
  const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(slot.csrf); ++i) {
    diff |= static_cast<uint8_t>(supplied[i * 2] ^ digits[slot.csrf[i] >> 4]);
    diff |= static_cast<uint8_t>(supplied[i * 2 + 1] ^ digits[slot.csrf[i] & 0x0F]);
  }
  if (diff != 0) {
    ++csrfFailures_;
    return false;
  }
  return true;
}

bool WebUi::issueSession(HttpdRequest& req, HttpdResponse& res) {
  const IPAddress ip = req.remoteIp();
  const uint32_t ipValue = static_cast<uint32_t>(ip[0]) |
                           (static_cast<uint32_t>(ip[1]) << 8) |
                           (static_cast<uint32_t>(ip[2]) << 16) |
                           (static_cast<uint32_t>(ip[3]) << 24);
  RuntimeConfig config;
  if (!configSnapshot(config))
    return false;

  const uint32_t now = millis();
  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    if (sessions_[i].inUse &&
        !WebUiSessionPolicy::isFresh(now, sessions_[i].issuedMs,
                                     config.webSessionTimeoutMs)) {
      sessions_[i].inUse = false;
    }
  }

  size_t slotIndex = MAX_SESSIONS;
  for (size_t i = 0; i < MAX_SESSIONS; ++i) {
    if (!sessions_[i].inUse) {
      slotIndex = i;
      break;
    }
  }
  if (slotIndex == MAX_SESSIONS) {
    slotIndex = 0;
    for (size_t i = 1; i < MAX_SESSIONS; ++i) {
      if (static_cast<int32_t>(sessions_[i].issuedMs - sessions_[slotIndex].issuedMs) < 0)
        slotIndex = i;
    }
    sessions_[slotIndex].inUse = false;
  }

  SessionSlot& slot = sessions_[slotIndex];
  for (size_t i = 0; i < sizeof(slot.secret); i += 4) {
    const uint32_t r = esp_random();
    memcpy(slot.secret + i, &r, min<size_t>(4, sizeof(slot.secret) - i));
  }
  for (size_t i = 0; i < sizeof(slot.csrf); i += 4) {
    const uint32_t r = esp_random();
    memcpy(slot.csrf + i, &r, min<size_t>(4, sizeof(slot.csrf) - i));
  }
  slot.issuedMs = now;
  slot.clientIp = ipValue;
  slot.inUse = true;
  activeSessionSlot_ = static_cast<int8_t>(slotIndex);

  uint8_t msg[8] = {};
  WebUiSessionPolicy::makeSessionMessage(ipValue, slot.issuedMs, msg);
  uint8_t token[32] = {};
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, slot.secret, sizeof(slot.secret),
                             msg, sizeof(msg), token, sizeof(token)) != 0) {
    slot.inUse = false;
    activeSessionSlot_ = -1;
    return false;
  }

  String hex;
  hex.reserve(64);
  const char* digits = "0123456789abcdef";
  for (uint8_t b : token) {
    hex += digits[b >> 4];
    hex += digits[b & 0x0F];
  }

  char cookie[160] = {};
  if (!WebUiSessionPolicy::buildSessionCookie(
          hex.c_str(), config.webSessionTimeoutMs / 1000, cookie,
          sizeof(cookie))) {
    slot.inUse = false;
    activeSessionSlot_ = -1;
    return false;
  }
  res.setHeader("Set-Cookie", cookie);
  return true;
}

String WebUi::csrfTokenHexForActiveSession() const {
  if (activeSessionSlot_ < 0 ||
      static_cast<size_t>(activeSessionSlot_) >= MAX_SESSIONS ||
      !sessions_[activeSessionSlot_].inUse)
    return String();
  const char* digits = "0123456789abcdef";
  String hex;
  hex.reserve(sizeof(sessions_[activeSessionSlot_].csrf) * 2);
  for (uint8_t b : sessions_[activeSessionSlot_].csrf) {
    hex += digits[b >> 4];
    hex += digits[b & 0x0F];
  }
  return hex;
}

void WebUi::auditAuth(HttpdRequest& req, bool success) {
  if (!storage.ready()) return;
  SpiLock spiLock(pdMS_TO_TICKS(50));
  if (!spiLock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  const char* path = "/LOG/WEB-AUTH.LOG";
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (f.size() >= Config::WEB_AUTH_LOG_ROTATE_BYTES) {
    f.close();
    const char* old = "/LOG/WEB-AUTH.1.LOG";
    if (SD.exists(old)) SD.remove(old);
    if (SD.exists(path)) SD.rename(path, old);
    f = SD.open(path, FILE_APPEND);
  }
  if (f) {
    const uint32_t now = millis();
    f.printf("%lu,%s,%s\n",
             static_cast<unsigned long>(now),
             req.remoteIp().toString().c_str(),
             success ? "AUTH_OK" : "AUTH_FAIL");
    f.close();
  }
}

bool WebUi::basicAuthMatches(HttpdRequest& req, const String& user,
                             const RuntimeConfig& config) {
  const String header = req.header("Authorization");
  if (!header.startsWith("Basic ")) return false;
  const String encoded = header.substring(6);
  if (encoded.length() == 0 || encoded.length() > 128) return false;

  uint8_t decoded[96] = {};
  size_t outLen = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &outLen,
                             reinterpret_cast<const uint8_t*>(encoded.c_str()),
                             encoded.length()) != 0)
    return false;
  decoded[outLen] = 0;
  const char* colon = reinterpret_cast<const char*>(memchr(decoded, ':', outLen));
  if (!colon) return false;
  const size_t userLen = static_cast<size_t>(colon - reinterpret_cast<const char*>(decoded));
  if (userLen != user.length() ||
      memcmp(decoded, user.c_str(), userLen) != 0)
    return false;
  const String password(reinterpret_cast<const char*>(colon + 1));
  return config.verifyWebPassword(password);
}

bool WebUi::auth(HttpdRequest& req, HttpdResponse& res) {
  const uint32_t now = millis();
  RuntimeConfig config;
  if (!configSnapshot(config)) {
    res.send503("configuration unavailable");
    return false;
  }

  constexpr uint32_t AUTH_ENTRY_TTL_MS = 5UL * 60UL * 1000UL;
  const String clientIp = req.remoteIp().toString();
  AuthThrottleEntry* entry = nullptr;
  AuthThrottleEntry* eviction = &authThrottle_[0];
  for (auto& candidate : authThrottle_) {
    const bool expired = !candidate.ip.isEmpty() &&
                         now - candidate.lastSeenMs >= AUTH_ENTRY_TTL_MS;
    if (candidate.ip == clientIp && !candidate.ip.isEmpty() && !expired) {
      entry = &candidate;
      break;
    }
    if (expired || candidate.lastSeenMs < eviction->lastSeenMs) eviction = &candidate;
  }
  if (!entry) {
    entry = eviction;
    *entry = AuthThrottleEntry{};
    entry->ip = clientIp;
  }
  entry->lastSeenMs = now;

  if (static_cast<int32_t>(now - entry->blockedUntilMs) < 0) {
    res.send429("too many authentication failures");
    return false;
  }

  if (static_cast<int32_t>(now - authBlockedUntilMs_) < 0) {
    res.send429("too many authentication failures");
    return false;
  }

  if (!sessionValid(req)) {
    if (!basicAuthMatches(req, config.webUser, config)) {
      if (now - entry->windowStartMs >= 60000U) {
        entry->windowStartMs = now;
        entry->failures = 0;
      }
      ++entry->failures;
      ++authFailures_;
      ++authFailureWindowCount_;
      auditAuth(req, false);

      if (entry->failures >= 5) {
        entry->blockedUntilMs = now + 30000U;
        entry->failures = 0;
      }
      if (now - authFailureWindowStartMs_ >= 60000U) {
        authFailureWindowStartMs_ = now;
        authFailures_ = 0;
      }
      if (authFailures_ >= 20) {
        authBlockedUntilMs_ = now + 30000U;
        authFailures_ = 0;
      }
      res.send401Basic("Login Required", "");
      return false;
    }

    entry->failures = 0;
    entry->windowStartMs = now;
    authFailures_ = 0;
    authFailureWindowCount_ = 0;
    authFailureWindowStartMs_ = now;
    if (!issueSession(req, res)) {
      auditAuth(req, false);
      res.send503("session initialization failed");
      return false;
    }
    auditAuth(req, true);
  }
  if (req.method() == HTTP_POST || req.method() == HTTP_DELETE) {
    if (config.csrfPolicy != 2 && config.csrfPolicy == 0 && !sameOrigin(req)) {
      res.send403("forbidden origin");
      return false;
    }
    if (config.csrfPolicy != 2 && !csrfValid(req)) {
      res.send403("invalid CSRF token");
      return false;
    }
  }
  return true;
}

// === SAMBUNG KE BAGIAN 3 (Pesan 6) ===
```

---

Status Setelah Pesan 5b

WebUi.cpp sekarang berisi:

1. ✅ Semua #include
2. ✅ INDEX_HTML lengkap
3. ✅ Free helpers: jsonEscape, hexEncodeUi, hexDecodeUi, configBackupPlaintext, backupKey, encryptConfigBackup, decryptConfigBackup, parseBackupLine, isValidUploadedWav, eraseStorageTree, sensorAddressJson, sensorNodeJson, auditConfigChange
4. ✅ extern deklarasi + gSensorSnapshots
5. ✅ begin() — 121 registrasi endpoint + server_.begin(...) TLS
6. ✅ task() — hanya record schedule
7. ✅ Auth/session/CSRF helpers: sameOrigin, rateLimit, findSessionSlot, sessionValid, csrfValid, issueSession, csrfTokenHexForActiveSession, auditAuth, basicAuthMatches, auth

Belum ada: 121 handler handleXxx(HttpdRequest&, HttpdResponse&).

Kalau Anda build sekarang, akan error undefined reference to WebUi::handleRoot, dst.

---

Setelah Pesan 5b Tersalin

File Status
HttpdServer.h/.cpp ✅ Pesan 1
HttpdRequest.h/.cpp ✅ Pesan 2
HttpdResponse.h/.cpp ✅ Pesan 2
HttpdMultipart.h/.cpp ✅ Pesan 3
WebUi.h ✅ Pesan 4
WebUi.cpp Bagian 1 ✅ Pesan 5
WebUi.cpp Bagian 2 ✅ Pesan 5b (ini)
WebUi.cpp Bagian 3 ⏳ Pesan 6 — semua handler
main.cpp diff ⏳ Pesan 7
platformio.ini diff ⏳ Pesan 7

Konfirmasi kalau Bagian 2 sudah tersalin, saya lanjut ke Pesan 6: Bagian 3 — handler 1 sampai ~60 (status, LoRaWAN, files, upload, messages, track, scan, hop, sensor).