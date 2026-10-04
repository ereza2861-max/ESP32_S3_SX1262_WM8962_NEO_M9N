#include "OtaApManager.h"

#ifndef SENSOR_NODE_ESP32C3
#error "OtaApManager.cpp is ESP32-C3-only"
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <Update.h>
#include <cctype>
#include <cstring>
#include <mbedtls/md.h>
#include <esp_system.h>
#include "webui_html.h"

namespace {
constexpr char NVS_NAMESPACE[] = "ota";
constexpr char NVS_KEY_AP_ENABLED[] = "ap_enabled";
constexpr uint16_t AP_PORT = 80;
constexpr const char* AP_SSID_PREFIX = "FieldRadio-Sensor-";
constexpr char PROFILE_PASSWORD_HEADER[] = "X-OTA-Password";
constexpr char PROFILE_SESSION_HEADER[] = "X-OTA-Session";
constexpr size_t OTA_PASSWORD_MIN_LEN = 12;
constexpr size_t OTA_PASSWORD_MAX_LEN = 64;
constexpr char PROFILE_CABLES_HEADER[] = "X-Profile-Cables-Changed";

bool validOtaPassword(const String& value) {
  if (value.length() < OTA_PASSWORD_MIN_LEN ||
      value.length() > OTA_PASSWORD_MAX_LEN) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c < 0x21 || c > 0x7E) return false;
  }
  return true;
}

WebServer* gServer = nullptr;

struct OtaCredentialBlob {
  uint32_t magic = 0x4F544132UL;  // "OTA2"
  uint8_t version = 1;
  uint8_t reserved[3] = {};
  uint32_t iterations = OtaApManager::PBKDF2_ITERATIONS;
  uint8_t salt[OtaApManager::PASSWORD_SALT_BYTES] = {};
  uint8_t hash[OtaApManager::PASSWORD_HASH_BYTES] = {};
};

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

bool pbkdf2Sha256(const String& password, const uint8_t* salt,
                  uint32_t iterations, uint8_t out[32]) {
  if (password.isEmpty() || !salt || !out || iterations < 100000U) return false;
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;

  uint8_t u[32] = {};
  uint8_t t[32] = {};
  uint8_t block[OtaApManager::PASSWORD_SALT_BYTES + 4] = {};
  std::memcpy(block, salt, OtaApManager::PASSWORD_SALT_BYTES);
  block[OtaApManager::PASSWORD_SALT_BYTES + 3] = 1;

  if (mbedtls_md_hmac(md, reinterpret_cast<const uint8_t*>(password.c_str()),
                      password.length(), block, sizeof(block), u, sizeof(u)) != 0)
    return false;
  std::memcpy(t, u, sizeof(t));

  for (uint32_t round = 1; round < iterations; ++round) {
    uint8_t next[32] = {};
    if (mbedtls_md_hmac(md, reinterpret_cast<const uint8_t*>(password.c_str()),
                        password.length(), u, sizeof(u), next, sizeof(next)) != 0)
      return false;
    std::memcpy(u, next, sizeof(u));
    for (size_t i = 0; i < sizeof(t); ++i) t[i] ^= u[i];
  }
  std::memcpy(out, t, sizeof(t));
  return true;
}

String hexBytes(const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

bool parseProfileBody(const String& body, uint8_t& profile) {
  String value = body;
  value.trim();
  if (!value.startsWith("{") || !value.endsWith("}")) return false;

  const int key = value.indexOf("\"profile\"");
  if (key < 0) return false;
  const int colon = value.indexOf(':', key + 9);
  if (colon < 0) return false;

  size_t pos = static_cast<size_t>(colon + 1);
  while (pos < value.length() && isspace(static_cast<unsigned char>(value[pos]))) ++pos;
  if (pos >= value.length() || value[pos] < '0' || value[pos] > '5') return false;

  const uint8_t parsed = static_cast<uint8_t>(value[pos] - '0');
  ++pos;
  while (pos < value.length() && isspace(static_cast<unsigned char>(value[pos]))) ++pos;
  if (pos >= value.length() || value[pos] != '}') return false;

  // Only the documented {profile: 0..5} payload is accepted.
  const String prefix = value.substring(0, key);
  if (prefix.indexOf('"') >= 0) return false;
  const String suffix = value.substring(pos + 1);
  if (suffix.length() != 0) return false;

  profile = parsed;
  return true;
}

String signingHexBytes(const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

bool decodeHex(const String& input, uint8_t* out, size_t len) {
  if (!out || input.length() != len * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = nibble(input[i * 2]);
    const int lo = nibble(input[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

struct OtaSigningBlob {
  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t reserved[3] = {};
  uint32_t iterations = 0;
  uint8_t salt[16] = {};
  uint8_t hash[32] = {};
};

bool loadOtaSigningMaterial(uint8_t salt[16], uint8_t hash[32],
                            uint32_t& iterations) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return false;
  OtaSigningBlob blob{};
  const bool ok =
      prefs.getBytes("opass.v2", &blob, sizeof(blob)) == sizeof(blob) &&
      blob.magic == 0x4F544132UL && blob.version == 1 &&
      blob.iterations >= 100000U;
  prefs.end();
  if (!ok) return false;
  std::memcpy(salt, blob.salt, 16);
  std::memcpy(hash, blob.hash, 32);
  iterations = blob.iterations;
  return true;
}

bool hmacSha256(const uint8_t key[32], const uint8_t* part1, size_t part1Len,
                const uint8_t* part2, size_t part2Len, uint8_t out[32]) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, md, 1) == 0 &&
            mbedtls_md_hmac_starts(&ctx, key, 32) == 0 &&
            mbedtls_md_hmac_update(&ctx, part1, part1Len) == 0 &&
            mbedtls_md_hmac_update(&ctx, part2, part2Len) == 0 &&
            mbedtls_md_hmac_finish(&ctx, out) == 0;
  mbedtls_md_free(&ctx);
  return ok;
}

// ArduinoOTA callbacks are free functions required by the ArduinoOTA API.
void onOtaStart() {
  Serial.println("OTA: ArduinoOTA update started");
}
void onOtaEnd() { Serial.println("OTA: ArduinoOTA update complete"); }
void onOtaProgress(unsigned int p, unsigned int t) {
  if (p == t || (p % 10 == 0)) Serial.printf("OTA: %u/%u\n", p, t);
}
void onOtaError(ota_error_t e) {
  Serial.printf("OTA: ArduinoOTA error %u\n", static_cast<unsigned>(e));
}

}  // namespace

bool OtaApManager::begin() {
  loadState();

  const bool requestedApAtBoot = apEnabled_;
  // loadState() stores the desired state, while startAp() uses apEnabled_
  // as the runtime "AP is actually up" guard. Clear the runtime flag before
  // starting so a persisted enabled state does not make startAp() return
  // without bringing the AP up.
  apEnabled_ = false;

  if (requestedApAtBoot && credentialValid_) {
    if (!startAp()) {
      Serial.println("WARN: AP start failed at boot; staying disabled");
      apEnabled_ = false;
      saveState();
    }
  }
  return true;
}

bool OtaApManager::beginRecoveryWindow() {
  if (!credentialValid_) {
    loadState();
  }
  if (!credentialValid_) {
    Serial.println("RECOVERY: OTA AP unavailable; provision OTA password over serial");
    return false;
  }
  if (!apEnabled_ && !startAp()) {
    Serial.println("RECOVERY: OTA AP start failed");
    return false;
  }
  return true;
}

void OtaApManager::loadState() {
  credentialValid_ = false;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  apEnabled_ = prefs.getBool(NVS_KEY_AP_ENABLED, false);

  OtaCredentialBlob blob{};
  if (prefs.getBytes("opass.v2", &blob, sizeof(blob)) == sizeof(blob) &&
      blob.magic == 0x4F544132UL && blob.version == 1 &&
      blob.iterations >= PBKDF2_ITERATIONS) {
    std::memcpy(passwordSalt_, blob.salt, sizeof(passwordSalt_));
    std::memcpy(passwordHash_, blob.hash, sizeof(passwordHash_));
    iterations_ = blob.iterations;
    credentialValid_ = true;
  }
  const String legacyPassword = credentialValid_ ? String() : prefs.getString("opass", "");
  prefs.end();

  // Explicit source-schema migration: convert the legacy plaintext key once,
  // then remove it. No plaintext credential is retained after migration.
  if (!credentialValid_ && validOtaPassword(legacyPassword))
    (void)provisionPassword(legacyPassword);
}

bool OtaApManager::provisionPassword(const String& password) {
  if (!validOtaPassword(password)) return false;

  OtaCredentialBlob blob{};
  blob.iterations = PBKDF2_ITERATIONS;
  for (size_t i = 0; i < sizeof(blob.salt); i += 4) {
    const uint32_t random = esp_random();
    std::memcpy(blob.salt + i, &random,
                (sizeof(blob.salt) - i) < sizeof(random)
                    ? sizeof(blob.salt) - i : sizeof(random));
  }
  if (!pbkdf2Sha256(password, blob.salt, blob.iterations, blob.hash)) return false;

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  const bool ok =
      prefs.putBytes("opass.v2", &blob, sizeof(blob)) == sizeof(blob);
  if (ok) (void)prefs.remove("opass");
  prefs.end();
  if (!ok) return false;

  std::memcpy(passwordSalt_, blob.salt, sizeof(passwordSalt_));
  std::memcpy(passwordHash_, blob.hash, sizeof(passwordHash_));
  iterations_ = blob.iterations;
  credentialValid_ = true;
  Serial.printf("OTA: credential provisioned; derived AP password=%s\\n",
                hexBytes(passwordHash_, sizeof(passwordHash_)).substring(0, 32).c_str());
  return true;
}

String OtaApManager::otaSigningSaltHex() const {
  return hexBytes(passwordSalt_, sizeof(passwordSalt_));
}

void OtaApManager::saveState() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBool(NVS_KEY_AP_ENABLED, apEnabled_);
  prefs.end();
}

bool OtaApManager::startAp() {
  if (apEnabled_) return true;
  const String ssid = String(AP_SSID_PREFIX) + String((uint32_t)ESP.getEfuseMac(), HEX);

  WiFi.mode(WIFI_AP);
  // The AP password is a derived secret because the plaintext OTA password
  // is no longer persisted. The first 32 hex chars are a valid WPA2 passphrase.
  const String derivedApPassword = hexBytes(passwordHash_, sizeof(passwordHash_)).substring(0, 32);
  if (!WiFi.softAP(ssid.c_str(), derivedApPassword.c_str())) {
    Serial.println("OTA: softAP start failed");
    return false;
  }
  delay(100);

  if (!gServer) gServer = new WebServer(AP_PORT);
  if (!gServer) return false;

  gServer->on("/", HTTP_GET, [this]() { handleRoot(); });
  gServer->on("/status", HTTP_GET, [this]() { handleStatus(); });
  gServer->on("/update", HTTP_POST,
              [this]() { handleUploadDone(); },
              [this]() { handleUpload(); });
  gServer->on("/profile", HTTP_POST, [this]() { handleProfile(); });
  gServer->on("/recovery/clear", HTTP_POST, [this]() { handleRecoveryClear(); });
  const char* headerKeys[] = {
      PROFILE_CABLES_HEADER, PROFILE_PASSWORD_HEADER, PROFILE_SESSION_HEADER,
      "X-OTA-Nonce", "X-OTA-Signature"};
  gServer->collectHeaders(headerKeys, 5);
  sessionToken_ = String(static_cast<uint32_t>(ESP.getEfuseMac() ^ micros()), HEX);
  gServer->begin();

  ArduinoOTA.setHostname(ssid.c_str());
  ArduinoOTA.setPassword(hexBytes(passwordHash_, sizeof(passwordHash_)).c_str());
  ArduinoOTA.onStart(onOtaStart);
  ArduinoOTA.onEnd(onOtaEnd);
  ArduinoOTA.onProgress(onOtaProgress);
  ArduinoOTA.onError(onOtaError);
  ArduinoOTA.begin();

  apEnabled_ = true;
  apStartedAtMs_ = millis();
  Serial.printf("OTA: AP up ssid=%s ip=%s window=%lus\n",
                ssid.c_str(),
                WiFi.softAPIP().toString().c_str(),
                static_cast<unsigned long>(OTA_AP_WINDOW_MS / 1000));
  return true;
}

void OtaApManager::stopAp() {
  if (!apEnabled_) return;
  ArduinoOTA.end();
  if (gServer) { gServer->stop(); delete gServer; gServer = nullptr; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apEnabled_ = false;
  clientConnected_ = false;
  Serial.println("OTA: AP down");
  saveState();
}

void OtaApManager::toggleAp() {
  if (apEnabled_) {
    stopAp();
  } else {
    if (!credentialValid_) {
      Serial.println("WARN: cannot enable AP: no OTA password provisioned");
      return;
    }
    startAp();
    saveState();
  }
}

void OtaApManager::task() {
  if (!apEnabled_) return;
  const uint32_t now = millis();

  if (gServer) gServer->handleClient();
  ArduinoOTA.handle();

  const int stationCount = WiFi.softAPgetStationNum();
  clientConnected_ = stationCount > 0;

  // The provisioning window is a hard upper bound. A connected client may
  // be observed for diagnostics, but it cannot keep the open AP alive
  // indefinitely.
  if (static_cast<uint32_t>(now - apStartedAtMs_) >= OTA_AP_WINDOW_MS) {
    Serial.println("OTA: provisioning window expired; stopping AP");
    stopAp();
  }
}

bool OtaApManager::checkOtaPassword(const String& supplied) {
  if (!credentialValid_ || !validOtaPassword(supplied)) return false;
  uint8_t candidate[PASSWORD_HASH_BYTES] = {};
  if (!pbkdf2Sha256(supplied, passwordSalt_, iterations_, candidate)) return false;
  return constantTimeEqual(candidate, passwordHash_, sizeof(candidate));
}

bool OtaApManager::checkSessionToken(const String& supplied) const {
  if (sessionToken_.isEmpty() || supplied.length() != sessionToken_.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < supplied.length(); ++i)
    diff |= static_cast<uint8_t>(supplied[i] ^ sessionToken_[i]);
  return diff == 0;
}

void OtaApManager::handleRoot() {
  if (!gServer) return;
  gServer->sendHeader("Cache-Control", "no-store");
  gServer->send_P(200, "text/html", WEBUI_HTML_PROGMEM);
}

void OtaApManager::handleStatus() {
  if (!gServer) return;
  String j = "{\"ap\":true,\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  j += ",\"windowMs\":" + String(OTA_AP_WINDOW_MS);
  j += ",\"elapsedMs\":" + String(millis() - apStartedAtMs_);
  j += ",\"otaReady\":" + String(credentialValid_ ? "true" : "false");
  j += ",\"session\":\"" + sessionToken_ + "\"";
  j += "}";
  gServer->send(200, "application/json", j);
}

bool OtaApManager::nonceSeen(const uint8_t nonce[16]) const {
  for (size_t i = 0; i < OTA_NONCE_CACHE_SIZE; ++i)
    if (std::memcmp(nonceCache_[i], nonce, OTA_NONCE_BYTES) == 0) return true;
  return false;
}

bool OtaApManager::rememberNonce(const uint8_t nonce[16]) {
  if (!nonce || nonceSeen(nonce)) return false;
  std::memcpy(nonceCache_[nonceCacheNext_], nonce, OTA_NONCE_BYTES);
  nonceCacheNext_ = (nonceCacheNext_ + 1U) % OTA_NONCE_CACHE_SIZE;
  return true;
}

bool OtaApManager::verifyRequestSignature(const String& nonceHex,
                                          const String& signatureHex,
                                          const uint8_t* body, size_t bodyLen) {
  uint8_t nonce[16] = {};
  uint8_t expected[32] = {};
  uint8_t key[32] = {};
  uint8_t salt[16] = {};
  uint32_t iterations = 0;
  if (!decodeHex(nonceHex, nonce, sizeof(nonce)) ||
      !decodeHex(signatureHex, expected, sizeof(expected)) ||
      nonceSeen(nonce) ||
      !loadOtaSigningMaterial(salt, key, iterations) ||
      !body) return false;
  (void)salt;
  (void)iterations;
  uint8_t actual[32] = {};
  if (!hmacSha256(key, nonce, sizeof(nonce), body, bodyLen, actual)) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(actual); ++i) diff |= actual[i] ^ expected[i];
  return diff == 0 && rememberNonce(nonce);
}

bool OtaApManager::beginUploadSignature(const String& nonceHex,
                                         const String& signatureHex) {
  uploadAuthorized_ = false;
  uint8_t nonce[16] = {};
  uint8_t key[32] = {};
  uint8_t salt[16] = {};
  uint32_t iterations = 0;
  if (!decodeHex(nonceHex, nonce, sizeof(nonce)) ||
      !decodeHex(signatureHex, uploadExpectedSignature_, sizeof(uploadExpectedSignature_)) ||
      nonceSeen(nonce) ||
      !loadOtaSigningMaterial(salt, key, iterations)) return false;
  (void)salt;
  (void)iterations;
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  if (uploadHmacActive_) mbedtls_md_free(&uploadHmac_);
  mbedtls_md_init(&uploadHmac_);
  if (mbedtls_md_setup(&uploadHmac_, md, 1) != 0 ||
      mbedtls_md_hmac_starts(&uploadHmac_, key, sizeof(key)) != 0 ||
      mbedtls_md_hmac_update(&uploadHmac_, nonce, sizeof(nonce)) != 0) {
    mbedtls_md_free(&uploadHmac_);
    return false;
  }
  std::memcpy(uploadNonce_, nonce, sizeof(uploadNonce_));
  uploadHmacActive_ = true;
  return true;
}

bool OtaApManager::updateUploadSignature(const uint8_t* data, size_t len) {
  return uploadHmacActive_ &&
         mbedtls_md_hmac_update(&uploadHmac_, data, len) == 0;
}

bool OtaApManager::finishUploadSignature() {
  if (!uploadHmacActive_) return false;
  uint8_t actual[32] = {};
  const bool ok = mbedtls_md_hmac_finish(&uploadHmac_, actual) == 0;
  mbedtls_md_free(&uploadHmac_);
  uploadHmacActive_ = false;
  if (!ok) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(actual); ++i)
    diff |= actual[i] ^ uploadExpectedSignature_[i];
  if (diff != 0) return false;
  uploadAuthorized_ = rememberNonce(uploadNonce_);
  return uploadAuthorized_;
}

void OtaApManager::handleProfile() {
  if (!gServer) return;
  if (!profileChangeCallback_) {
    gServer->send(503, "application/json",
                  "{\"ok\":false,\"error\":\"profile handler unavailable\"}");
    return;
  }
  if (gServer->header(PROFILE_CABLES_HEADER) != "true") {
    gServer->send(409, "application/json",
                  "{\"ok\":false,\"error\":\"confirm physical sensor cables were replaced\"}");
    return;
  }
  if (!checkOtaPassword(gServer->header(PROFILE_PASSWORD_HEADER)) ||
      !checkSessionToken(gServer->header(PROFILE_SESSION_HEADER))) {
    gServer->send(401, "application/json",
                  "{\"ok\":false,\"error\":\"OTA authentication required\"}");
    return;
  }
  const String profileBody = gServer->arg("plain");
  if (!verifyRequestSignature(gServer->header("X-OTA-Nonce"),
                              gServer->header("X-OTA-Signature"),
                              reinterpret_cast<const uint8_t*>(profileBody.c_str()),
                              profileBody.length())) {
    gServer->send(401, "application/json",
                  "{\"ok\":false,\"error\":\"invalid OTA request signature\"}");
    return;
  }

  uint8_t profile = 0;
  if (!parseProfileBody(gServer->arg("plain"), profile)) {
    gServer->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"body must be {\\\"profile\\\":0..5}\"}");
    return;
  }

  if (!profileChangeCallback_(profile)) {
    gServer->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"profile change rejected\"}");
    return;
  }

  gServer->send(200, "application/json",
                "{\"ok\":true,\"rebooting\":true}");
  delay(100);
  ESP.restart();
}

void OtaApManager::handleRecoveryClear() {
  if (!recoveryClearCallback_) {
    gServer->send(503, "application/json", "{\"ok\":false,\"error\":\"unavailable\"}");
    return;
  }
  const String supplied = gServer->header(PROFILE_PASSWORD_HEADER);
  if (!checkOtaPassword(supplied)) {
    gServer->send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  if (!recoveryClearCallback_()) {
    gServer->send(500, "application/json", "{\"ok\":false,\"error\":\"persist_failed\"}");
    return;
  }
  gServer->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  delay(50);
  ESP.restart();
}

void OtaApManager::handleUpload() {
  if (!gServer) return;
  HTTPUpload& upload = gServer->upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA: WebUI upload start %s\n", upload.filename.c_str());
    // SECURITY: require OTA password and the time-bounded OTA session.
    const String supplied = gServer->arg("password");
    const String session = gServer->header(PROFILE_SESSION_HEADER);
    if (!checkOtaPassword(supplied) || !checkSessionToken(session) ||
        !beginUploadSignature(gServer->header("X-OTA-Nonce"),
                               gServer->header("X-OTA-Signature"))) {
      Serial.println("OTA: WebUI upload rejected (bad password, session, or signature)");
      upload.status = UPLOAD_FILE_ABORTED;
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.println("OTA: Update.begin failed");
      upload.status = UPLOAD_FILE_ABORTED;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!updateUploadSignature(upload.buf, upload.currentSize) ||
        Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.println("OTA: Update.write failed");
      upload.status = UPLOAD_FILE_ABORTED;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!finishUploadSignature()) {
      Serial.println("OTA: firmware signature verification failed");
      upload.status = UPLOAD_FILE_ABORTED;
      return;
    }
    if (!Update.end(true)) {
      Serial.printf("OTA: Update.end failed: %s\n", Update.errorString());
      uploadAuthorized_ = false;
    }
  }
}

void OtaApManager::handleUploadDone() {
  if (!gServer) return;
  const bool authorized =
      uploadAuthorized_ &&
      checkOtaPassword(gServer->arg("password")) &&
      checkSessionToken(gServer->header(PROFILE_SESSION_HEADER));
  if (!authorized) {
    gServer->send(401, "text/plain", "ERROR: OTA authentication required");
    return;
  }
  const bool ok = !Update.hasError();
  gServer->send(ok ? 200 : 500, "text/plain",
                ok ? "OK: firmware updated, rebooting" : "ERROR: upload failed");
  if (ok) {
    delay(200);
    ESP.restart();
  }
}
