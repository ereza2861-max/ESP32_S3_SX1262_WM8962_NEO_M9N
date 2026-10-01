#include "MqttClientManager.h"
#include "SensorSpool.h"
#include "Config.h"
#include "PersistentConfig.h"
#include "MqttCaCert.h"
#include "Telemetry.h"
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <SD.h>
#include "AppState.h"
#include "StorageManager.h"
#ifndef CONFIG_SECURE_BOOT_V2_ENABLED
#define CONFIG_SECURE_BOOT_V2_ENABLED 0
#endif
#ifndef CONFIG_SECURE_FLASH_ENC_ENABLED
#define CONFIG_SECURE_FLASH_ENC_ENABLED 0
#endif

extern StorageManager storage;

namespace {
constexpr char NVS_NS[] = "mqtt_creds";
uint64_t mqttCertTimeToEpoch(const mbedtls_x509_time& t) {
  if (t.year < 1970 || t.mon < 1 || t.mon > 12 || t.day < 1 || t.day > 31 ||
      t.hour > 23 || t.min > 59 || t.sec > 59) return 0;
  const int y = t.year;
  const unsigned m = t.mon;
  const unsigned d = t.day;
  const int yAdj = y - (m <= 2);
  const int era = (yAdj >= 0 ? yAdj : yAdj - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(yAdj - era * 400);
  const unsigned mp = static_cast<unsigned>(static_cast<int>(m) + (m > 2 ? -3 : 9));
  const unsigned doy = (153U * mp + 2U) / 5U + d - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097LL +
                       static_cast<int64_t>(doe) - 719468LL;
  if (days < 0) return 0;
  return static_cast<uint64_t>(days) * 86400ULL +
         static_cast<uint64_t>(t.hour) * 3600ULL +
         static_cast<uint64_t>(t.min) * 60ULL +
         static_cast<uint64_t>(t.sec);
}

String mqttCertName(const mbedtls_x509_name* name) {
  char buf[512] = {};
  if (!name || mbedtls_x509_dn_gets(buf, sizeof(buf), name) < 0) return String();
  return String(buf);
}

String mqttCertSerial(const mbedtls_x509_crt& crt) {
  static const char hex[] = "0123456789ABCDEF";
  String out = "0x";
  for (size_t i = 0; i < crt.serial.len; ++i) {
    out += hex[crt.serial.p[i] >> 4];
    out += hex[crt.serial.p[i] & 0x0F];
  }
  return out;
}


constexpr uint8_t CERT_SLOT_COMMIT = 0xA7;
bool loadCommittedCertificateSlot(Preferences& prefs, String& cert, String& key) {
  String ca = prefs.getString("cert_a", "");
  String ka = prefs.getString("key_a", "");
  String cb = prefs.getString("cert_b", "");
  String kb = prefs.getString("key_b", "");
  const bool va = prefs.getUChar("cert_commit_a", 0) == CERT_SLOT_COMMIT &&
                  !ca.isEmpty() && !ka.isEmpty();
  const bool vb = prefs.getUChar("cert_commit_b", 0) == CERT_SLOT_COMMIT &&
                  !cb.isEmpty() && !kb.isEmpty();
  if (!va && !vb) return false;
  const uint32_t ga = va ? prefs.getUInt("cert_gen_a", 0) : 0;
  const uint32_t gb = vb ? prefs.getUInt("cert_gen_b", 0) : 0;
  if (gb > ga) { cert = cb; key = kb; }
  else { cert = ca; key = ka; }
  return true;
}

bool parseMqttCertificateMetadata(const String& pem, uint64_t& expiry,
                                  String& subject, String& issuer, String& serial) {
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  const int rc = mbedtls_x509_crt_parse(
      &crt, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.length() + 1U);
  if (rc != 0) {
    mbedtls_x509_crt_free(&crt);
    return false;
  }
  expiry = mqttCertTimeToEpoch(crt.valid_to);
  subject = mqttCertName(&crt.subject);
  issuer = mqttCertName(&crt.issuer);
  serial = mqttCertSerial(crt);
  mbedtls_x509_crt_free(&crt);
  return expiry != 0;
}

constexpr time_t MIN_VALID_EPOCH = 1700000000;
constexpr size_t MQTT_MAX_BLOB = 1024;
constexpr char CRED_MAGIC[] = "FRMQ1";
}

String MqttClientManager::topic(const char* leaf) const {
  String out = Config::MQTT_TOPIC_ROOT;
  out += '/';
  out += Config::DEVICE_ID;
  if (leaf && *leaf) {
    out += '/';
    out += leaf;
  }
  return out;
}

bool MqttClientManager::timeSynchronized() const {
  return time(nullptr) >= MIN_VALID_EPOCH;
}

bool deriveCredentialKey(uint8_t key[32]) {
  if (!key) return false;
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
  const char label[] = "FieldRadio MQTT credential encryption v1";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0 &&
            mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1) == 0 &&
            mbedtls_sha256_update(&ctx, mac, sizeof(mac)) == 0 &&
            mbedtls_sha256_finish(&ctx, key) == 0;
  mbedtls_sha256_free(&ctx);
  return ok;
}

String mqttHexEncode(const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0x0f];
  }
  return out;
}

bool mqttHexDecode(const String& in, uint8_t* out, size_t len) {
  if (!out || in.length() != len * 2) return false;
  auto n = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = n(in[i * 2]), lo = n(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool MqttClientManager::encryptCredentials(String& envelope) const {
  uint8_t key[32] = {}, iv[16] = {}, mac[32] = {};
  if (!deriveCredentialKey(key)) return false;
  for (size_t i = 0; i < sizeof(iv); i += 4) {
    const uint32_t r = esp_random();
    memcpy(iv + i, &r, min<size_t>(4, sizeof(iv) - i));
  }
  String plain = host_ + "|" + String(port_) + "|" + user_ + "|" + pass_;
  if (plain.length() > 512) return false;

  uint8_t* cipher = static_cast<uint8_t*>(malloc(plain.length() ? plain.length() : 1));
  if (!cipher) return false;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t ncOff = 0;
  uint8_t stream[16] = {};
  bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
            mbedtls_aes_crypt_ctr(&aes, plain.length(), &ncOff, iv, stream,
                                  reinterpret_cast<const unsigned char*>(plain.c_str()), cipher) == 0;
  mbedtls_aes_free(&aes);
  if (!ok) { free(cipher); return false; }

  envelope = CRED_MAGIC;
  envelope += "|";
  envelope += mqttHexEncode(iv, sizeof(iv));
  envelope += "|";
  envelope += mqttHexEncode(cipher, plain.length());
  free(cipher);

  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  ok = md && mbedtls_md_hmac(md, key, sizeof(key),
                             reinterpret_cast<const uint8_t*>(envelope.c_str()),
                             envelope.length(), mac, sizeof(mac)) == 0;
  if (!ok) return false;
  envelope += "|";
  envelope += mqttHexEncode(mac, sizeof(mac));
  return envelope.length() <= MQTT_MAX_BLOB;
}

bool MqttClientManager::decryptCredentials(const String& envelope) {
  if (envelope.length() < 20 || envelope.length() > MQTT_MAX_BLOB) return false;
  const int p1 = envelope.indexOf('|');
  const int p2 = p1 >= 0 ? envelope.indexOf('|', p1 + 1) : -1;
  const int p3 = p2 >= 0 ? envelope.indexOf('|', p2 + 1) : -1;
  if (p1 != static_cast<int>(strlen(CRED_MAGIC)) || p2 <= p1 || p3 <= p2 ||
      envelope.substring(0, p1) != CRED_MAGIC) return false;

  uint8_t key[32] = {}, iv[16] = {}, expected[32] = {}, supplied[32] = {};
  if (!deriveCredentialKey(key) ||
      !mqttHexDecode(envelope.substring(p1 + 1, p2), iv, sizeof(iv)) ||
      !mqttHexDecode(envelope.substring(p3 + 1), supplied, sizeof(supplied)))
    return false;
  const String signedPart = envelope.substring(0, p3);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md_hmac(md, key, sizeof(key),
                             reinterpret_cast<const uint8_t*>(signedPart.c_str()),
                             signedPart.length(), expected, sizeof(expected)) != 0)
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(expected); ++i) diff |= expected[i] ^ supplied[i];
  if (diff != 0) return false;

  const String cipherHex = envelope.substring(p2 + 1, p3);
  if ((cipherHex.length() & 1U) != 0 || cipherHex.length() > 1024) return false;
  const size_t len = cipherHex.length() / 2;
  uint8_t* plain = static_cast<uint8_t*>(malloc(len + 1));
  uint8_t* cipher = static_cast<uint8_t*>(malloc(len ? len : 1));
  if (!plain || !cipher || !mqttHexDecode(cipherHex, cipher, len)) {
    free(plain); free(cipher); return false;
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t ncOff = 0;
  uint8_t stream[16] = {};
  bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
            mbedtls_aes_crypt_ctr(&aes, len, &ncOff, iv, stream, cipher, plain) == 0;
  mbedtls_aes_free(&aes);
  free(cipher);
  if (!ok) { free(plain); return false; }
  plain[len] = 0;

  String decoded(reinterpret_cast<char*>(plain));
  free(plain);
  const int a = decoded.indexOf('|');
  const int b = a >= 0 ? decoded.indexOf('|', a + 1) : -1;
  const int c = b >= 0 ? decoded.indexOf('|', b + 1) : -1;
  if (a <= 0 || b <= a || c <= b || c == static_cast<int>(decoded.length()) - 1) return false;
  const long port = decoded.substring(a + 1, b).toInt();
  if (port <= 0 || port > 65535) return false;
  host_ = decoded.substring(0, a);
  user_ = decoded.substring(b + 1, c);
  pass_ = decoded.substring(c + 1);
  return !host_.isEmpty() && host_.length() <= 253 && user_.length() <= 128 && pass_.length() <= 128;
}

bool MqttClientManager::loadCredentials() {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  credentialsProvisioned_ = prefs.getBool("provisioned", false);
  if (!loadCommittedCertificateSlot(prefs, clientCertificatePem_, clientPrivateKeyPem_)) {
    clientCertificatePem_ = prefs.getString("cert", "");
    clientPrivateKeyPem_ = prefs.getString("key", "");
  }
  pkiProvisioned_ = !clientCertificatePem_.isEmpty() && !clientPrivateKeyPem_.isEmpty();
  passwordProvisionedEpoch_ = static_cast<time_t>(prefs.getLong64("pass_epoch", 0));
  certExpiryEpoch_ = static_cast<uint64_t>(prefs.getLong64("cert_expiry", 0));
  certSubject_ = prefs.getString("cert_subject", "");
  certIssuer_ = prefs.getString("cert_issuer", "");
  certSerial_ = prefs.getString("cert_serial", "");
  prefs.end();

  if (pkiProvisioned_) {
    uint64_t expiry = 0;
    String subject, issuer, serial;
    if (!parseMqttCertificateMetadata(clientCertificatePem_, expiry, subject, issuer, serial)) {
      pkiProvisioned_ = false;
      return false;
    }
    certExpiryEpoch_ = expiry;
    certSubject_ = subject;
    certIssuer_ = issuer;
    certSerial_ = serial;
    // F1-TODO-2: Keep rotation metadata in RAM only; source it from the active certificate.
    rotation_.currentSerial = certSerial_;
    rotation_.expiresAt = certExpiryEpoch_;
    Preferences meta;
    if (meta.begin(NVS_NS, false)) {
      (void)meta.putLong64("cert_expiry", static_cast<int64_t>(certExpiryEpoch_));
      (void)meta.putString("cert_subject", certSubject_);
      (void)meta.putString("cert_issuer", certIssuer_);
      (void)meta.putString("cert_serial", certSerial_);
      meta.end();
    }
  }

  // D-06: the broker credential authority is the device-specific X.509
  // certificate/private key. Username/password is no longer a connection
  // prerequisite and is not used by the MQTT task.
  if (!credentialsProvisioned_ || !pkiProvisioned_) return false;
  host_ = config.mqttHost;
  port_ = config.mqttPort;
  if (!config.mqttTlsRequired || host_.isEmpty() || port_ == 0) return false;
  return true;
}


bool MqttClientManager::saveCredentials() {
  String blob;
  if (!encryptCredentials(blob)) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  uint64_t expiry = certExpiryEpoch_;
  String subject = certSubject_, issuer = certIssuer_, serial = certSerial_;
  if (!parseMqttCertificateMetadata(clientCertificatePem_, expiry, subject, issuer, serial)) return false;
  certExpiryEpoch_ = expiry;
  certSubject_ = subject;
  certIssuer_ = issuer;
  certSerial_ = serial;
  const bool ok = prefs.putString("blob", blob) > 0 &&
                  prefs.putString("cert", clientCertificatePem_) > 0 &&
                  prefs.putString("key", clientPrivateKeyPem_) > 0 &&
                  prefs.putBool("provisioned", true) &&
                  prefs.putLong64("pass_epoch", static_cast<int64_t>(timeSynchronized() ? time(nullptr) : 0)) > 0 &&
                  prefs.putLong64("cert_expiry", static_cast<int64_t>(certExpiryEpoch_)) > 0 &&
                  prefs.putString("cert_subject", certSubject_) > 0 &&
                  prefs.putString("cert_issuer", certIssuer_) > 0 &&
                  prefs.putString("cert_serial", certSerial_) > 0;
  prefs.end();
  if (ok) {
    credentialsProvisioned_ = true;
    passwordProvisionedEpoch_ = timeSynchronized() ? time(nullptr) : 0;
  }
  return ok;
}

bool MqttClientManager::provisionCredentials(const String& host, uint16_t port,
                                             const String& user, const String& pass) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (host.isEmpty() || host.length() > 253 || port == 0 ||
      user.length() > 128 || pass.length() > 128 ||
      host.indexOf('|') >= 0 || user.indexOf('|') >= 0 || pass.indexOf('|') >= 0) return false;
  if (Config::mqttTlsIsMandatory() && port == 1883) return false;
  // Username/password provisioning is no longer an accepted production path.
  // Use provisionCertificate() so the device has a unique client certificate.
  (void)host; (void)port; (void)user; (void)pass;
  return false;

  plain_.stop();
  secure_.stop();
  useTls_ = config.mqttTlsRequired;
  if (Config::mqttTlsIsMandatory() && !config.mqttTlsRequired) return false;
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
    if (!pkiProvisioned_ || clientCertificatePem_.isEmpty() || clientPrivateKeyPem_.isEmpty()) return false;
    secure_.setCertificate(clientCertificatePem_.c_str());
    secure_.setPrivateKey(clientPrivateKeyPem_.c_str());
    secure_.setHandshakeTimeout(10);
    client_.setClient(secure_);
  } else {
    client_.setClient(plain_);
  }
  client_.setServer(host_.c_str(), port_);
  connected_ = false;
  nextRetryMs_ = 0;
  retryDelayMs_ = config.mqttReconnectMinMs;
  auditEvent("PROVISIONED");
  return true;
}

bool MqttClientManager::provisionCertificate(const String& host, uint16_t port,
                                                const String& certificatePem,
                                                const String& privateKeyPem) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (host.isEmpty() || host.length() > 253 || port == 0 ||
      certificatePem.length() < 64 || certificatePem.length() > 8192 ||
      privateKeyPem.length() < 64 || privateKeyPem.length() > 8192 ||
      certificatePem.indexOf("-----BEGIN CERTIFICATE-----") < 0 ||
      certificatePem.indexOf("-----END CERTIFICATE-----") < 0 ||
      privateKeyPem.indexOf("-----BEGIN") < 0 ||
      privateKeyPem.indexOf("PRIVATE KEY-----") < 0) {
    return false;
  }
  if (Config::mqttTlsIsMandatory() && (port == 1883 || !config.mqttTlsRequired)) return false;

  uint64_t expiry = 0;
  String subject, issuer, serial;
  if (!parseMqttCertificateMetadata(certificatePem, expiry, subject, issuer, serial)) return false;
  host_ = host;
  port_ = port;
  clientCertificatePem_ = certificatePem;
  clientPrivateKeyPem_ = privateKeyPem;
  certExpiryEpoch_ = expiry;
  certSubject_ = subject;
  certIssuer_ = issuer;
  certSerial_ = serial;
  pkiProvisioned_ = true;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  const bool stored = prefs.putString("cert", clientCertificatePem_) > 0 &&
                      prefs.putString("key", clientPrivateKeyPem_) > 0 &&
                      prefs.putBool("provisioned", true) &&
                      prefs.putLong64("pass_epoch",
                                      static_cast<int64_t>(timeSynchronized() ? time(nullptr) : 0)) > 0 &&
                      prefs.putLong64("cert_expiry", static_cast<int64_t>(certExpiryEpoch_)) > 0 &&
                      prefs.putString("cert_subject", certSubject_) > 0 &&
                      prefs.putString("cert_issuer", certIssuer_) > 0 &&
                      prefs.putString("cert_serial", certSerial_) > 0 &&
                      prefs.putBool("cert_enrolled", false);
  prefs.end();
  if (!stored) return false;
  credentialsProvisioned_ = true;
  passwordProvisionedEpoch_ = timeSynchronized() ? time(nullptr) : 0;

  plain_.stop();
  secure_.stop();
  useTls_ = true;
  secure_.setCACert(MQTT_BROKER_ROOT_CA);
  secure_.setCertificate(clientCertificatePem_.c_str());
  secure_.setPrivateKey(clientPrivateKeyPem_.c_str());
  secure_.setHandshakeTimeout(10);
  client_.setClient(secure_);
  client_.setServer(host_.c_str(), port_);
  connected_ = false;
  nextRetryMs_ = 0;
  retryDelayMs_ = config.mqttReconnectMinMs;
  auditEvent("PKI_PROVISIONED");
  return true;
}

// F1-TODO-4: Convert the RAM phase value to a stable audit/debug name.
const char* MqttClientManager::rotationPhaseName(uint8_t phase) {
  switch (static_cast<RotationPhase>(phase)) {
    case RotationPhase::Idle: return "idle";
    case RotationPhase::Requesting: return "requesting";
    case RotationPhase::Verifying: return "verifying";
    case RotationPhase::Installing: return "installing";
    case RotationPhase::Reconnecting: return "reconnecting";
    case RotationPhase::Failed: return "failed";
    default: return "unknown";
  }
}

// F1-TODO-3: Only inspect certificate expiry; no HTTP or MQTT rotation request is made.
void MqttClientManager::serviceRotation() {
  if (!credentialsProvisioned_) return;
  const uint32_t now = millis();
  if (now - rotation_.lastCheckMs < ROTATION_CHECK_INTERVAL_MS) return;
  rotation_.lastCheckMs = now;
  if (rotation_.expiresAt == 0) return;

  const time_t epoch = time(nullptr);
  if (epoch < MIN_VALID_EPOCH) {
    // F1-TODO-3: UNSPECIFIED: epoch source beyond the existing time() API is not defined.
    return;
  }
  const uint64_t nowEpoch = static_cast<uint64_t>(epoch);
  const uint64_t daysLeft =
      nowEpoch >= rotation_.expiresAt
          ? 0ULL
          : (rotation_.expiresAt - nowEpoch) / 86400ULL;

  RuntimeConfig config{};
  if (!configSnapshot(config)) return;
  if (daysLeft <= config.certRenewalThresholdDays) {
    rotation_.rotationPending = true;
    // F1-TODO-10: Audit rotation event ROTATION_DUE.
    auditEvent("ROTATION_DUE");
  }
}

// F1-TODO-6: Verify the candidate X.509 material without persisting it.
bool MqttClientManager::verifyNewCertificate(const String& certPem,
                                             const String& caPem,
                                             const String& expectedSerial,
                                             uint64_t expectedExpiresAt) {
  rotation_.phase = static_cast<uint8_t>(RotationPhase::Verifying);
  // F1-TODO-10: Audit rotation event ROTATION_VERIFY_START.
  auditEvent("ROTATION_VERIFY_START");

  auto fail = [this]() {
    rotation_.phase = static_cast<uint8_t>(RotationPhase::Failed);
    ++rotation_.retryCount;
    // F1-TODO-10: Audit rotation event ROTATION_VERIFY_FAIL.
    auditEvent("ROTATION_VERIFY_FAIL");
    if (rotation_.retryCount >= ROTATION_MAX_RETRY) {
      rotation_.phase = static_cast<uint8_t>(RotationPhase::Idle);
    }
    return false;
  };

  if (certPem.length() < 64 || certPem.length() > 8192 ||
      caPem.length() < 64 || caPem.length() > 8192 ||
      expectedSerial.isEmpty() || expectedExpiresAt == 0) {
    return fail();
  }

  mbedtls_x509_crt cert;
  mbedtls_x509_crt ca;
  mbedtls_x509_crt_init(&cert);
  mbedtls_x509_crt_init(&ca);
  bool ok = false;

  do {
    if (mbedtls_x509_crt_parse(
            &cert, reinterpret_cast<const unsigned char*>(certPem.c_str()),
            certPem.length() + 1U) != 0) break;
    if (mbedtls_x509_crt_parse(
            &ca, reinterpret_cast<const unsigned char*>(caPem.c_str()),
            caPem.length() + 1U) != 0) break;
    if (cert.ca_istrue != 0 || ca.ca_istrue == 0) break;
    if (mqttCertName(&cert.issuer) != mqttCertName(&ca.subject)) break;
    if (!mqttCertSerial(cert).equalsIgnoreCase(expectedSerial)) break;
    if (time(nullptr) < MIN_VALID_EPOCH) break;
    if (mbedtls_x509_time_is_past(&cert.valid_to) ||
        mbedtls_x509_time_is_future(&cert.valid_from)) break;
    const uint64_t notAfter = mqttCertTimeToEpoch(cert.valid_to);
    if (notAfter == 0 ||
        (notAfter > expectedExpiresAt
             ? notAfter - expectedExpiresAt
             : expectedExpiresAt - notAfter) > 300ULL) break;
    ok = true;
  } while (false);

  mbedtls_x509_crt_free(&ca);
  mbedtls_x509_crt_free(&cert);

  if (!ok) return fail();

  rotation_.pendingSerial = expectedSerial;
  rotation_.phase = static_cast<uint8_t>(RotationPhase::Idle);
  rotation_.retryCount = 0;
  // F1-TODO-10: Audit rotation event ROTATION_VERIFY_OK.
  auditEvent("ROTATION_VERIFY_OK");
  return true;
}

// F1-TODO-7/9: Install new certificate material behind a durable rollback journal.
bool MqttClientManager::installPendingCertificate(const String& certPem,
                                                  const String& keyPem,
                                                  const String& caPem,
                                                  const String& serial) {
  if (certPem.isEmpty() || keyPem.isEmpty() || caPem.isEmpty() || serial.isEmpty()) {
    // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
    auditEvent("ROTATION_INSTALL_FAIL");
    return false;
  }
  uint64_t newExpiry = 0;
  String newSubject, newIssuer, newSerial;
  if (!parseMqttCertificateMetadata(certPem, newExpiry, newSubject, newIssuer, newSerial) ||
      !newSerial.equalsIgnoreCase(serial)) {
    // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
    auditEvent("ROTATION_INSTALL_FAIL");
    return false;
  }

  if (!loadCredentials()) {
    // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
    auditEvent("ROTATION_INSTALL_FAIL");
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) {
    // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
    auditEvent("ROTATION_INSTALL_FAIL");
    return false;
  }
  const String oldCert = clientCertificatePem_;
  const String oldKey = clientPrivateKeyPem_;
  const String oldCa = prefs.getString("cert_ca", "");
  const String oldSerial = certSerial_;
  bool journalOk =
      prefs.putString("rot_prev_cert", oldCert) > 0 &&
      prefs.putString("rot_prev_key", oldKey) > 0 &&
      prefs.putString("rot_prev_ca", oldCa) > 0 &&
      prefs.putString("rot_prev_serial", oldSerial) > 0 &&
      prefs.putUChar("rot_state", 1) == sizeof(uint8_t);
  prefs.end();
  if (!journalOk) {
    // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
    auditEvent("ROTATION_INSTALL_FAIL");
    return false;
  }

  if (!prefs.begin(NVS_NS, false)) {
    (void)rollbackPendingInstall();
    // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
    auditEvent("ROTATION_INSTALL_FAIL");
    return false;
  }
  const bool stored =
      prefs.putString("cert", certPem) > 0 &&
      prefs.putString("key", keyPem) > 0 &&
      prefs.putString("cert_ca", caPem) > 0 &&
      prefs.putString("cert_serial", serial) > 0 &&
      prefs.putLong64("cert_expiry", static_cast<int64_t>(newExpiry)) > 0;
  if (stored) {
    const String verifyCert = prefs.getString("cert", "");
    const String verifyKey = prefs.getString("key", "");
    const String verifyCa = prefs.getString("cert_ca", "");
    const String verifySerial = prefs.getString("cert_serial", "");
    if (verifyCert != certPem || verifyKey != keyPem ||
        verifyCa != caPem || verifySerial != serial) {
      prefs.end();
      (void)rollbackPendingInstall();
      // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
      auditEvent("ROTATION_INSTALL_FAIL");
      return false;
    }
  }
  if (!stored || prefs.putUChar("rot_state", 2) != sizeof(uint8_t)) {
    prefs.end();
    (void)rollbackPendingInstall();
    // F1-TODO-10: Audit rotation event ROTATION_INSTALL_FAIL.
    auditEvent("ROTATION_INSTALL_FAIL");
    return false;
  }
  (void)prefs.remove("rot_prev_cert");
  (void)prefs.remove("rot_prev_key");
  (void)prefs.remove("rot_prev_ca");
  (void)prefs.remove("rot_prev_serial");
  (void)prefs.remove("rot_state");
  prefs.end();
  // F1-TODO-10: Audit rotation event ROTATION_INSTALL_OK.
  auditEvent("ROTATION_INSTALL_OK");
  return true;
}

// F1-TODO-9: Restore the journaled MQTT certificate material after a failed install.
bool MqttClientManager::rollbackPendingInstall() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  const String oldCert = prefs.getString("rot_prev_cert", "");
  const String oldKey = prefs.getString("rot_prev_key", "");
  const String oldCa = prefs.getString("rot_prev_ca", "");
  const String oldSerial = prefs.getString("rot_prev_serial", "");
  if (prefs.putUChar("rot_state", 3) != sizeof(uint8_t)) {
    prefs.end();
    return false;
  }
  const bool restored =
      prefs.putString("cert", oldCert) > 0 &&
      prefs.putString("key", oldKey) > 0 &&
      prefs.putString("cert_ca", oldCa) > 0 &&
      prefs.putString("cert_serial", oldSerial) > 0;
  if (!restored) {
    prefs.end();
    return false;
  }
  (void)prefs.remove("rot_prev_cert");
  (void)prefs.remove("rot_prev_key");
  (void)prefs.remove("rot_prev_ca");
  (void)prefs.remove("rot_prev_serial");
  const bool cleared = prefs.putUChar("rot_state", 0) == sizeof(uint8_t);
  prefs.end();
  if (cleared) {
    // F1-TODO-10: Audit rotation event ROTATION_ROLLBACK_OK.
    auditEvent("ROTATION_ROLLBACK_OK");
  }
  return cleared;
}

bool MqttClientManager::passwordRotationWarning() const {
  // D-06 uses certificate lifecycle; password rotation is retained only as a
  // compatibility API and is never used as an MQTT authentication policy.
  return false;
}

bool MqttClientManager::reloadCertificateMaterial() {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  String cert, key;
  if (!loadCommittedCertificateSlot(prefs, cert, key)) {
    cert = prefs.getString("cert", "");
    key = prefs.getString("key", "");
  }
  prefs.end();
  if (cert.isEmpty() || key.isEmpty()) return false;
  uint64_t expiry = 0;
  String subject, issuer, serial;
  if (!parseMqttCertificateMetadata(cert, expiry, subject, issuer, serial)) return false;
  clientCertificatePem_ = cert;
  clientPrivateKeyPem_ = key;
  certExpiryEpoch_ = expiry;
  certSubject_ = subject;
  certIssuer_ = issuer;
  certSerial_ = serial;
  if (config.mqttTlsRequired) {
    secure_.stop();
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
    secure_.setCertificate(clientCertificatePem_.c_str());
    secure_.setPrivateKey(clientPrivateKeyPem_.c_str());
    secure_.setHandshakeTimeout(10);
    client_.setClient(secure_);
  }
  connected_ = false;
  client_.disconnect();
  nextRetryMs_ = 0;
  retryDelayMs_ = config.mqttReconnectMinMs;
  return true;
}

bool MqttClientManager::replaceConnectionCredentials(const String& previousSerial) {
  if (previousSerial.isEmpty()) return false;
  RuntimeConfig config{};
  if (!configSnapshot(config) || !config.mqttTlsRequired) return false;
  if (!loadCredentials() || !pkiProvisioned_) return false;
  plain_.stop();
  secure_.stop();
  client_.disconnect();
  secure_.setCACert(MQTT_BROKER_ROOT_CA);
  secure_.setCertificate(clientCertificatePem_.c_str());
  secure_.setPrivateKey(clientPrivateKeyPem_.c_str());
  secure_.setHandshakeTimeout(10);
  client_.setClient(secure_);
  client_.setServer(host_.c_str(), port_);
  connected_ = false;
  nextRetryMs_ = 0;
  retryDelayMs_ = config.mqttReconnectMinMs;
  rotationPreviousSerial_ = previousSerial;
  rotationRetirementPending_ = true;
  auditEvent("ROTATION_CONNECTION_REPLACEMENT");
  return true;
}

bool MqttClientManager::retirePreviousCredential() {
  if (!rotationRetirementPending_ || rotationPreviousSerial_.isEmpty()) return true;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  bool removed = false;
  for (const char* suffix : {"a", "b"}) {
    const String certKey = String("cert_") + suffix;
    const String keyKey = String("key_") + suffix;
    const String commitKey = String("cert_commit_") + suffix;
    const String cert = prefs.getString(certKey.c_str(), "");
    const String key = prefs.getString(keyKey.c_str(), "");
    const bool committed = prefs.getUChar(commitKey.c_str(), 0) == CERT_SLOT_COMMIT;
    if (!committed || cert.isEmpty() || key.isEmpty()) continue;
    uint64_t expiry = 0;
    String subject, issuer, serial;
    if (!parseMqttCertificateMetadata(cert, expiry, subject, issuer, serial)) continue;
    if (!serial.equalsIgnoreCase(rotationPreviousSerial_)) continue;
    const bool slotRemoved =
        prefs.remove(certKey.c_str()) &&
        prefs.remove(keyKey.c_str()) &&
        prefs.remove(commitKey.c_str()) &&
        prefs.remove((String("cert_gen_") + suffix).c_str());
    if (!slotRemoved) {
      prefs.end();
      auditEvent("ROTATION_OLD_CREDENTIAL_RETIRE_FAIL");
      return false;
    }
    removed = true;
  }
  // Legacy single-slot material is no longer a recovery credential once a
  // replacement has connected successfully.
  const bool legacyRemoved = prefs.remove("cert") && prefs.remove("key");
  prefs.end();
  if (!legacyRemoved) {
    auditEvent("ROTATION_OLD_CREDENTIAL_RETIRE_FAIL");
    return false;
  }
  rotationPreviousSerial_.clear();
  rotationRetirementPending_ = false;
  auditEvent(removed ? "ROTATION_OLD_CREDENTIAL_RETIRED" : "ROTATION_OLD_CREDENTIAL_ALREADY_RETIRED");
  return true;
}

void MqttClientManager::auditEvent(const char* event, int mqttState) {
  if (!storage.ready() || !event) return;
  SpiLock lock(pdMS_TO_TICKS(50));
  if (!lock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  const char* path = "/LOG/MQTT-AUTH.LOG";
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (f.size() >= 64UL * 1024UL) {
    f.close();
    const char* old = "/LOG/MQTT-AUTH.1.LOG";
    if (SD.exists(old)) SD.remove(old);
    (void)SD.rename(path, old);
    f = SD.open(path, FILE_APPEND);
  }
  if (f) {
    f.printf("%lu,%s,%d,%s\n", static_cast<unsigned long>(millis()),
             host_.c_str(), mqttState, event);
    f.close();
  }
}


bool MqttClientManager::begin() {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (Config::mqttTlsIsMandatory() && !config.mqttTlsRequired) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "MQTT TLS is mandatory in this build";
    return false;
  }
  enabled_ = config.mqttEnabled;
  if (!enabled_) {
    connected_ = false;
    client_.disconnect();
    return true;
  }
  if (!sensorQueue_) {
    sensorQueue_ = xQueueCreateStatic(16, sizeof(SensorSample),
                                      sensorQueueStorage_, &sensorQueueStruct_);
    if (!sensorQueue_) return false;
  }
  client_.setKeepAlive(30);
  client_.setSocketTimeout(2);
  if (!loadCredentials()) return false;

  plain_.stop();
  secure_.stop();
  useTls_ = config.mqttTlsRequired;
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
    if (!pkiProvisioned_ || clientCertificatePem_.isEmpty() || clientPrivateKeyPem_.isEmpty()) return false;
    secure_.setCertificate(clientCertificatePem_.c_str());
    secure_.setPrivateKey(clientPrivateKeyPem_.c_str());
    secure_.setHandshakeTimeout(10);
    client_.setClient(secure_);
  } else {
    client_.setClient(plain_);
  }
  client_.setServer(host_.c_str(), port_);
  return true;
}

bool MqttClientManager::connect(const String& host, uint16_t port,
                                const String& user, const String& pass) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (host.isEmpty() || host.length() > 253 || port == 0 ||
      user.length() > 128 || pass.length() > 128) return false;
  host_ = host; port_ = port; user_ = user; pass_ = pass;
  plain_.stop();
  secure_.stop();
  useTls_ = config.mqttTlsRequired;
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
    if (!pkiProvisioned_ || clientCertificatePem_.isEmpty() || clientPrivateKeyPem_.isEmpty()) return false;
    secure_.setCertificate(clientCertificatePem_.c_str());
    secure_.setPrivateKey(clientPrivateKeyPem_.c_str());
    secure_.setHandshakeTimeout(10);
    client_.setClient(secure_);
  } else {
    client_.setClient(plain_);
  }
  client_.setServer(host_.c_str(), port_);
  return provisionCredentials(host, port, user, pass);
}

bool MqttClientManager::publish(const String& topic, const String& payload, bool retained) {
  if (!enabled_ || !isConnected() || topic.isEmpty() || payload.isEmpty()) return false;
  if (topic.length() > 128 || payload.length() > 2048) return false;
  return client_.publish(topic.c_str(), payload.c_str(), retained);
}

bool MqttClientManager::publishSensorData(uint32_t nodeId, const char* nodeName,
                                            uint16_t sensorId, const char* sensorName,
                                            const char* unit, float value, uint8_t quality,
                                            int16_t rssi, uint64_t timestampMs) {
  if (!sensorQueue_ || nodeId == 0 || sensorId == 0 || !nodeName || !sensorName ||
      !unit || !std::isfinite(value)) return false;
  SensorSample sample{};
  sample.nodeId = nodeId;
  sample.sensorId = sensorId;
  sample.value = value;
  sample.quality = quality;
  sample.rssi = rssi;
  sample.timestampMs = timestampMs;
  std::strncpy(sample.nodeName, nodeName, sizeof(sample.nodeName) - 1);
  std::strncpy(sample.sensorName, sensorName, sizeof(sample.sensorName) - 1);
  std::strncpy(sample.unit, unit, sizeof(sample.unit) - 1);
  // BLE callback path: zero-timeout enqueue only; MQTT/TCP work stays in task().
  return xQueueSend(sensorQueue_, &sample, 0) == pdTRUE;
}

Client& MqttClientManager::mqttTransport() {
  return useTls_ ? static_cast<Client&>(secure_) : static_cast<Client&>(plain_);
}

size_t MqttClientManager::encodeMqttRemainingLength(uint8_t* out, size_t length) {
  if (!out || length > 268435455UL) return 0;
  size_t count = 0;
  do {
    uint8_t encoded = static_cast<uint8_t>(length % 128U);
    length /= 128U;
    if (length > 0) encoded |= 0x80U;
    out[count++] = encoded;
  } while (length > 0 && count < 4);
  return count;
}

bool MqttClientManager::waitForPubAck(uint16_t packetId, uint32_t timeoutMs) {
  Client& transport = mqttTransport();
  const uint32_t deadline = millis() + timeoutMs;
  uint8_t packetBytes[2] = {};

  while (static_cast<int32_t>(millis() - deadline) < 0) {
    if (!transport.connected()) return false;
    if (!transport.available()) {
      delay(1);
      continue;
    }

    const int first = transport.read();
    if (first < 0) continue;
    const uint8_t header = static_cast<uint8_t>(first);
    size_t multiplier = 1;
    size_t remainingLength = 0;
    bool completeLength = false;
    for (uint8_t i = 0; i < 4; ++i) {
      const int byte = transport.read();
      if (byte < 0) return false;
      remainingLength += static_cast<size_t>(byte & 0x7f) * multiplier;
      if ((byte & 0x80) == 0) {
        completeLength = true;
        break;
      }
      multiplier *= 128;
    }
    if (!completeLength || remainingLength > 2) return false;

    if ((header & 0xf0U) == 0x40U && remainingLength == 2U) {
      if (transport.readBytes(packetBytes, sizeof(packetBytes)) != sizeof(packetBytes)) return false;
      const uint16_t ackId = static_cast<uint16_t>(packetBytes[0] << 8 | packetBytes[1]);
      return ackId == packetId;
    }

    // PubSubClient owns inbound MQTT dispatch. A sensor QoS-1 publish must not
    // consume unrelated broker traffic while waiting for its PUBACK. Any
    // unexpected packet makes the current transport state ambiguous, so force
    // reconnect rather than acknowledging the spool record.
    return false;
  }
  return false;
}

bool MqttClientManager::publishSensorSampleQos1(const String& mqttTopic, const String& payload) {
  if (mqttTopic.isEmpty() || payload.isEmpty() || mqttTopic.length() > 128 || payload.length() > 2048) {
    return false;
  }

  Client& transport = mqttTransport();
  if (!transport.connected()) return false;

  const size_t remainingLength = 2U + mqttTopic.length() + 2U + payload.length();
  uint8_t remaining[4] = {};
  const size_t remainingLengthBytes = encodeMqttRemainingLength(remaining, remainingLength);
  if (remainingLengthBytes == 0) return false;

  uint16_t packetId = nextPacketId_++;
  if (packetId == 0) packetId = nextPacketId_++;

  const auto writeFully = [&transport](const uint8_t* data, size_t len) -> bool {
    return transport.write(data, len) == len;
  };
  const uint8_t fixedHeader = 0x32U;
  if (!writeFully(&fixedHeader, 1) || !writeFully(remaining, remainingLengthBytes)) {
    client_.disconnect();
    connected_ = false;
    return false;
  }
  const uint8_t topicLength[2] = {
      static_cast<uint8_t>((mqttTopic.length() >> 8) & 0xffU),
      static_cast<uint8_t>(mqttTopic.length() & 0xffU)};
  const uint8_t packetIdBytes[2] = {
      static_cast<uint8_t>((packetId >> 8) & 0xffU),
      static_cast<uint8_t>(packetId & 0xffU)};
  if (!writeFully(topicLength, sizeof(topicLength)) ||
      !writeFully(reinterpret_cast<const uint8_t*>(mqttTopic.c_str()), mqttTopic.length()) ||
      !writeFully(packetIdBytes, sizeof(packetIdBytes)) ||
      !writeFully(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length())) {
    client_.disconnect();
    connected_ = false;
    return false;
  }
  transport.flush();
  if (!waitForPubAck(packetId, 2000)) {
    client_.disconnect();
    connected_ = false;
    return false;
  }
  return true;
}

bool MqttClientManager::publishSensorSample(const SensorSample& sample) {
  auto escapeJson = [](const char* in) -> String {
    String out;
    if (!in) return out;
    out.reserve(std::strlen(in) + 8);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p; ++p) {
      switch (*p) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (*p < 0x20) {
            char hex[7] = {};
            std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned>(*p));
            out += hex;
          } else out += static_cast<char>(*p);
      }
    }
    return out;
  };

  const String nodeName = escapeJson(sample.nodeName);
  const String sensorName = escapeJson(sample.sensorName);
  const String unit = escapeJson(sample.unit);
  char valueText[32] = {};
  std::snprintf(valueText, sizeof(valueText), "%.9g", static_cast<double>(sample.value));
  String payload;
  payload.reserve(256);
  payload += "{\"ts\":";
  payload += String(static_cast<unsigned long long>(sample.timestampMs));
  payload += ",\"node\":";
  payload += String(static_cast<unsigned long>(sample.nodeId));
  payload += ",\"node_name\":\"";
  payload += nodeName;
  payload += "\",\"sensor\":";
  payload += String(static_cast<unsigned>(sample.sensorId));
  payload += ",\"sensor_name\":\"";
  payload += sensorName;
  payload += "\",\"unit\":\"";
  payload += unit;
  payload += "\",\"value\":";
  payload += valueText;
  payload += ",\"sample_id\":";
  payload += String(static_cast<unsigned long>(SensorSpool::sampleId(sample)));
  payload += ",\"quality\":";
  payload += String(static_cast<unsigned>(sample.quality));
  payload += ",\"rssi\":";
  payload += String(static_cast<int>(sample.rssi));
  payload += '}';
  if (payload.length() > 512) return false;

  String sensorLeaf = "sensor/";
  sensorLeaf += String(static_cast<unsigned long>(sample.nodeId));
  sensorLeaf += '/';
  sensorLeaf += String(static_cast<unsigned>(sample.sensorId));
  return publishSensorSampleQos1(topic(sensorLeaf.c_str()), payload);
}

void MqttClientManager::setEnabled(bool enabled) {
  if (enabled_ == enabled) {
    if (enabled_) (void)applyConfig();
    return;
  }
  enabled_ = enabled;
  if (!enabled_) {
    connected_ = false;
    client_.disconnect();
    secure_.stop();
    plain_.stop();
    return;
  }
  (void)applyConfig();
}

bool MqttClientManager::applyConfig() {
  RuntimeConfig config;
  if (!configSnapshot(config)) return false;
  return applyConfig(config);
}

bool MqttClientManager::applyConfig(const RuntimeConfig& config) {
  if (Config::mqttTlsIsMandatory() && !config.mqttTlsRequired) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "MQTT TLS is mandatory in this build";
    return false;
  }
  if (!config.mqttEnabled) {
    if (enabled_) setEnabled(false);
    return true;
  }
  enabled_ = true;
  const bool endpointChanged =
      host_ != config.mqttHost ||
      port_ != config.mqttPort ||
      useTls_ != config.mqttTlsRequired;
  if (!loadCredentials()) return false;
  if (endpointChanged || !client_.connected()) {
    connected_ = false;
    client_.disconnect();
    secure_.stop();
    plain_.stop();
  }
  useTls_ = config.mqttTlsRequired;
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
    if (!pkiProvisioned_ || clientCertificatePem_.isEmpty() || clientPrivateKeyPem_.isEmpty()) return false;
    secure_.setCertificate(clientCertificatePem_.c_str());
    secure_.setPrivateKey(clientPrivateKeyPem_.c_str());
    secure_.setHandshakeTimeout(10);
    client_.setClient(secure_);
  } else {
    client_.setClient(plain_);
  }
  client_.setServer(host_.c_str(), port_);
  nextRetryMs_ = 0;
  retryDelayMs_ = config.mqttReconnectMinMs;
  return !host_.isEmpty() && port_ != 0;
}

void MqttClientManager::task() {
  RuntimeConfig config;
  if (!enabled_ || !configSnapshot(config)) return;
  if (host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
    if (connected_) auditEvent("DISCONNECT", client_.state());
    connected_ = false;
    return;
  }

  static bool ntpRequested = false;
  if (!ntpRequested) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ntpRequested = true;
  }
  if (!timeSynchronized()) return;
  // F1-TODO-3: Run rotation readiness synchronously in the existing MQTT task.
  serviceRotation();
  if (certExpiryEpoch_ != 0 && static_cast<uint64_t>(time(nullptr)) >= certExpiryEpoch_) {
    connected_ = false;
    client_.disconnect();
    auditEvent("CERT_EXPIRED");
    return;
  }

  if (!client_.connected()) {
    if (connected_) auditEvent("DISCONNECT", client_.state());
    connected_ = false;
    if (nextRetryMs_ != 0 &&
        static_cast<int32_t>(millis() - nextRetryMs_) < 0) return;
    nextRetryMs_ = 0;

    const String clientId = Config::DEVICE_ID;
    const String willTopic = topic("availability");
    const char* willPayload = "offline";
    bool ok = false;
    if (useTls_) {
      // PubSubClient does not expose a separate SNI hostname. Pre-connect the
      // ESP32 secure client with the broker hostname; NetworkClientSecure uses
      // that hostname for TLS SNI/certificate verification, and PubSubClient
      // then reuses the already-connected transport for the MQTT handshake.
      // ASSUMPTION: MQTT_HOST is the TLS SNI hostname.
      // Trade-off: a distinct SNI override would require a custom secure-client
      // transport rather than this small integration.
      if (!secure_.connected()) {
        if (!secure_.connect(host_.c_str(), port_)) {
          nextRetryMs_ = millis() + retryDelayMs_;
          retryDelayMs_ = min<uint32_t>(config.mqttReconnectMaxMs, retryDelayMs_ * 2U);
          return;
        }
      }
    }
    // Final MQTT authority is certificate-based. The legacy username/password
    // blob is retained only for migration/rollback bookkeeping; it is never
    // selected for the production connection path.
    ok = pkiProvisioned_ && client_.connect(clientId.c_str(), nullptr, nullptr,
                                            willTopic.c_str(), 0,
                                            config.mqttRetainAvailability,
                                            willPayload, true);
    if (ok) {
      connected_ = true;
      auditEvent("CONNECT_OK", client_.state());
      retryDelayMs_ = config.mqttReconnectMinMs;
      publish(willTopic, "online", config.mqttRetainAvailability);
      if (rotationRetirementPending_ && !retirePreviousCredential()) {
        auditEvent("ROTATION_OLD_CREDENTIAL_RETIRE_FAIL");
      }
    } else {
      if (client_.state() == MQTT_CONNECT_BAD_CREDENTIALS) auditEvent("AUTH_FAIL", client_.state());
      else auditEvent("CONNECT_FAIL", client_.state());
      retryDelayMs_ = min<uint32_t>(config.mqttReconnectMaxMs, retryDelayMs_ * 2U);
      nextRetryMs_ = millis() + retryDelayMs_;
    }
    return;
  }

  connected_ = client_.loop();
  if (connected_ && sensorQueue_) {
    SensorSample sample{};
    while (xQueuePeek(sensorQueue_, &sample, 0) == pdTRUE) {
      if (!publishSensorSample(sample)) break;
      (void)xQueueReceive(sensorQueue_, &sample, 0);
    }
  }
  static uint32_t lastPublishMs = 0;
  if (connected_ && millis() - lastPublishMs >= config.mqttTelemetryPeriodMs) {
    const String payload = makeLoRaWANUplinkJson();
    if (!payload.isEmpty() &&
        publish(topic("telemetry"), payload, config.mqttRetainTelemetry)) {
      lastPublishMs = millis();
    }
  }

  static uint32_t lastHealthMs = 0;
  if (connected_ && millis() - lastHealthMs >= config.mqttHealthPeriodMs) {
    const String health = String("{\"uptime_ms\":") + String(millis()) +
                          ",\"free_heap\":" + String(ESP.getFreeHeap()) +
                          ",\"mqtt_connected\":true}";
    if (publish(topic("health"), health, config.mqttRetainAvailability))
      lastHealthMs = millis();
  }
}
