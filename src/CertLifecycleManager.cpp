#include "CertLifecycleManager.h"
#include "MqttClientManager.h"
#include "PersistentConfig.h"
#include "Config.h"
#include "AppState.h"
#include "StorageManager.h"
#include "EstCaCert.h"
#include <Preferences.h>
#include <SD.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <algorithm>
#include <cstring>

extern StorageManager storage;

namespace {
constexpr char NVS_NS[] = "mqtt_creds";
constexpr time_t MIN_VALID_EPOCH = 1700000000;
constexpr uint32_t MANUAL_RATE_LIMIT_MS = 60000UL;
constexpr uint32_t MAX_CERT_CHAIN = 32768U;

bool parsePemCert(const String& pem, mbedtls_x509_crt& crt) {
  return mbedtls_x509_crt_parse(&crt,
                                reinterpret_cast<const unsigned char*>(pem.c_str()),
                                pem.length() + 1U) == 0;
}

String serialHex(const mbedtls_x509_crt& crt) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(crt.serial.len * 2U + 2U);
  out += "0x";
  for (size_t i = 0; i < crt.serial.len; ++i) {
    out += hex[crt.serial.p[i] >> 4];
    out += hex[crt.serial.p[i] & 0x0F];
  }
  return out;
}

String x509Name(const mbedtls_x509_name* name) {
  char buf[512] = {};
  if (!name || mbedtls_x509_dn_gets(buf, sizeof(buf), name) < 0) return String();
  return String(buf);
}
}

CertLifecycleManager::CertLifecycleManager(MqttClientManager& mqtt) : mqtt_(mqtt) {}

bool CertLifecycleManager::begin() {
  if (mutex_) return true;
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;
  loadPersistentState();
  RuntimeConfig cfg;
  if (configSnapshot(cfg) && cfg.estAuthMode == 2 && cfg.estBootstrapTokenConsumed)
    enrolled_ = true;
  if (parseCertificate(mqtt_.clientCertificatePem(), false)) audit("VALIDATED");
  nextCheckMs_ = millis() + 1000UL;
  started_ = true;
  return true;
}

bool CertLifecycleManager::enabled() const {
  RuntimeConfig cfg;
  return configSnapshot(cfg) && cfg.certLifecycleEnabled;
}

uint64_t CertLifecycleManager::timeToEpoch(const mbedtls_x509_time& t) {
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

bool CertLifecycleManager::clockValid() const {
  StateLock lock(gState);
  return lock.ok() && gState.gps.timeValid && gState.gps.utcEpoch >= MIN_VALID_EPOCH;
}

bool CertLifecycleManager::parseCertificate(const String& pem, bool verifyChain) {
  if (pem.isEmpty() || pem.length() > MAX_CERT_CHAIN) return false;
  mbedtls_x509_crt chain;
  mbedtls_x509_crt trust;
  mbedtls_x509_crt_init(&chain);
  mbedtls_x509_crt_init(&trust);
  bool ok = false;
  do {
    if (!parsePemCert(pem, chain)) break;
    const uint64_t nb = timeToEpoch(chain.valid_from);
    const uint64_t na = timeToEpoch(chain.valid_to);
    if (!nb || !na || na <= nb) break;
    if (verifyChain) {
      if (mbedtls_x509_crt_parse(&trust,
                                 reinterpret_cast<const unsigned char*>(estTrustAnchor()),
                                 strlen(estTrustAnchor()) + 1U) != 0) break;
      uint32_t flags = 0;
      if (mbedtls_x509_crt_verify(&chain, &trust, nullptr, nullptr, &flags, nullptr, nullptr) != 0 || flags != 0) break;
    }
    expiryEpoch_ = na;
    notBeforeEpoch_ = nb;
    subject_ = x509Name(&chain.subject);
    issuer_ = x509Name(&chain.issuer);
    serial_ = serialHex(chain);
    ok = true;
  } while (false);
  mbedtls_x509_crt_free(&trust);
  mbedtls_x509_crt_free(&chain);
  return ok;
}

bool CertLifecycleManager::loadPersistentState() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;
  expiryEpoch_ = static_cast<uint64_t>(p.getLong64("cert_expiry", 0));
  lastRenewalMs_ = p.getUInt("cert_last_ms", 0);
  renewalFailures_ = p.getUShort("cert_failures", 0);
  serial_ = p.getString("cert_serial", serial_);
  issuer_ = p.getString("cert_issuer", issuer_);
  lastRenewalStatus_ = p.getString("cert_last_status", "NEVER");
  enrolled_ = p.getBool("cert_enrolled", false);
  p.end();
  return true;
}

bool CertLifecycleManager::savePersistentState() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;
  bool ok = p.putLong64("cert_expiry", static_cast<int64_t>(expiryEpoch_)) > 0;
  ok = ok && p.putUInt("cert_last_ms", lastRenewalMs_) > 0;
  ok = ok && p.putUShort("cert_failures", renewalFailures_) > 0;
  ok = ok && p.putString("cert_serial", serial_) > 0;
  ok = ok && p.putString("cert_issuer", issuer_) > 0;
  ok = ok && p.putString("cert_last_status", lastRenewalStatus_) > 0;
  ok = ok && p.putBool("cert_enrolled", enrolled_);
  p.end();
  return ok;
}

bool CertLifecycleManager::atomicStore(const String& certPem, const String& keyPem) {
  if (certPem.isEmpty() || keyPem.isEmpty() ||
      certPem.length() > 8192 || keyPem.length() > 8192) return false;
  constexpr uint8_t COMMIT = 0xA7;
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;
  const bool va = p.getUChar("cert_commit_a", 0) == COMMIT &&
                  !p.getString("cert_a", "").isEmpty() &&
                  !p.getString("key_a", "").isEmpty();
  const bool vb = p.getUChar("cert_commit_b", 0) == COMMIT &&
                  !p.getString("cert_b", "").isEmpty() &&
                  !p.getString("key_b", "").isEmpty();
  const uint32_t ga = va ? p.getUInt("cert_gen_a", 0) : 0;
  const uint32_t gb = vb ? p.getUInt("cert_gen_b", 0) : 0;
  const bool writeA = !va || (vb && gb >= ga);
  const char* certKey = writeA ? "cert_a" : "cert_b";
  const char* keyKey = writeA ? "key_a" : "key_b";
  const char* commitKey = writeA ? "cert_commit_a" : "cert_commit_b";
  const char* genKey = writeA ? "cert_gen_a" : "cert_gen_b";
  const uint32_t currentGen = std::max(ga, gb);
  const uint32_t nextGen = currentGen == UINT32_MAX ? 1U : currentGen + 1U;
  (void)p.remove(commitKey);
  bool ok = p.putString(certKey, certPem) > 0 &&
            p.putString(keyKey, keyPem) > 0 &&
            p.getString(certKey, "") == certPem &&
            p.getString(keyKey, "") == keyPem;
  if (ok) ok = p.putUInt(genKey, nextGen) > 0;
  if (ok) ok = p.putUChar(commitKey, COMMIT) == sizeof(uint8_t);
  if (ok) {
    ok = p.getUChar(commitKey, 0) == COMMIT &&
         p.getUInt(genKey, 0) == nextGen &&
         p.getString(certKey, "") == certPem &&
         p.getString(keyKey, "") == keyPem;
  }
  p.end();
  return ok;
}

bool CertLifecycleManager::verifyAndInstall(const String& certChainPem, const String& newKeyPem, String& selectedCertPem) {
  if (certChainPem.isEmpty() || newKeyPem.isEmpty()) return false;
  mbedtls_x509_crt chain;
  mbedtls_x509_crt_init(&chain);
  bool ok = false;
  do {
    if (!parsePemCert(certChainPem, chain)) break;
    RuntimeConfig cfg;
    if (!configSnapshot(cfg)) break;
    mbedtls_x509_crt selected;
    mbedtls_x509_crt_init(&selected);
    String currentSubject;
    int cursor = 0;
    bool foundDeviceCert = false;
    while (cursor >= 0) {
      const int begin = certChainPem.indexOf("-----BEGIN CERTIFICATE-----", cursor);
      if (begin < 0) break;
      const int end = certChainPem.indexOf("-----END CERTIFICATE-----", begin);
      if (end < begin) break;
      const String candidatePem = certChainPem.substring(begin, end + strlen("-----END CERTIFICATE-----")) + "\n";
      mbedtls_x509_crt candidate;
      mbedtls_x509_crt_init(&candidate);
      if (parsePemCert(candidatePem, candidate)) {
        const String candidateSubject = x509Name(&candidate.subject);
        if (candidateSubject.indexOf(cfg.callsign) >= 0 ||
            candidateSubject.indexOf(Config::DEVICE_ID) >= 0) {
          selectedCertPem = candidatePem;
          currentSubject = candidateSubject;
          mbedtls_x509_crt_free(&selected);
          selected = candidate;
          foundDeviceCert = true;
          break;
        }
      }
      mbedtls_x509_crt_free(&candidate);
      cursor = end + 1;
    }
    if (!foundDeviceCert) {
      mbedtls_x509_crt_free(&selected);
      break;
    }
    mbedtls_pk_context privateKey;
    mbedtls_pk_init(&privateKey);
    if (mbedtls_pk_parse_key(&privateKey,
        reinterpret_cast<const unsigned char*>(newKeyPem.c_str()), newKeyPem.length() + 1U,
        nullptr, 0, nullptr, nullptr) != 0 ||
        mbedtls_pk_check_pair(&selected.pk, &privateKey) != 0) {
      mbedtls_pk_free(&privateKey);
      mbedtls_x509_crt_free(&selected);
      break;
    }
    String verificationPem = selectedCertPem;
    int appendCursor = 0;
    while (appendCursor >= 0) {
      const int b = certChainPem.indexOf("-----BEGIN CERTIFICATE-----", appendCursor);
      if (b < 0) break;
      const int e = certChainPem.indexOf("-----END CERTIFICATE-----", b);
      if (e < b) break;
      const String candidatePem = certChainPem.substring(b, e + strlen("-----END CERTIFICATE-----")) + "\n";
      if (candidatePem != selectedCertPem && verificationPem.length() + candidatePem.length() <= MAX_CERT_CHAIN)
        verificationPem += candidatePem;
      appendCursor = e + 1;
    }
    mbedtls_x509_crt verificationChain;
    mbedtls_x509_crt_init(&verificationChain);
    mbedtls_x509_crt trust;
    mbedtls_x509_crt_init(&trust);
    if (!parsePemCert(verificationPem, verificationChain) ||
        mbedtls_x509_crt_parse(&trust,
          reinterpret_cast<const unsigned char*>(estTrustAnchor()),
          strlen(estTrustAnchor()) + 1U) != 0) {
      mbedtls_x509_crt_free(&trust); mbedtls_x509_crt_free(&verificationChain);
      mbedtls_x509_crt_free(&selected); break;
    }
    uint32_t flags = 0;
    const int verifyRc = mbedtls_x509_crt_verify(&verificationChain, &trust, nullptr, nullptr,
                                                 &flags, nullptr, nullptr);
    mbedtls_x509_crt_free(&trust);
    mbedtls_x509_crt_free(&verificationChain);
    if (verifyRc != 0 || flags != 0) {
      mbedtls_pk_free(&privateKey);
      mbedtls_x509_crt_free(&selected);
      break;
    }
    mbedtls_pk_free(&privateKey);
    audit("ROTATION_BROKER_CREDENTIAL_UPDATE_ACCEPTED");
    if (!atomicStore(selectedCertPem, newKeyPem)) {
      mbedtls_x509_crt_free(&selected);
      break;
    }
    audit("ROTATION_SECURE_PERSISTENCE_COMMITTED");
    if (!mqtt_.replaceConnectionCredentials(mqtt_.getCertSerial())) {
      mbedtls_x509_crt_free(&selected);
      break;
    }
    expiryEpoch_ = timeToEpoch(selected.valid_to);
    notBeforeEpoch_ = timeToEpoch(selected.valid_from);
    subject_ = currentSubject;
    issuer_ = x509Name(&selected.issuer);
    serial_ = serialHex(selected);
    mbedtls_x509_crt_free(&selected);
    ok = expiryEpoch_ > notBeforeEpoch_;
  } while (false);
  mbedtls_x509_crt_free(&chain);
  return ok;
}

bool CertLifecycleManager::renewCertificate(bool manual) {
  if (!started_ || !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool ok = false;
  do {
    RuntimeConfig cfg;
    if (!configSnapshot(cfg) || !cfg.certLifecycleEnabled || cfg.estServerUrl.isEmpty()) break;
    if (!clockValid()) { lastRenewalStatus_ = "CLOCK_INVALID"; break; }
    static uint32_t lastManualMs = 0;
    if (manual && lastManualMs != 0 && static_cast<int32_t>(millis() - lastManualMs) < static_cast<int32_t>(MANUAL_RATE_LIMIT_MS)) {
      lastRenewalStatus_ = "RATE_LIMITED";
      break;
    }
    if (manual) lastManualMs = millis();

    audit("ROTATION_DEADLINE_CHECK");
    String oldCert = mqtt_.clientCertificatePem();
    String oldKey = mqtt_.clientPrivateKeyPem();
    if (cfg.estAuthMode == 0 && (oldCert.isEmpty() || oldKey.isEmpty())) {
      lastRenewalStatus_ = "NO_BOOTSTRAP_CERT";
      break;
    }
    if (cfg.estAuthMode == 1 && (cfg.estUsername.isEmpty() || cfg.estPassword.isEmpty())) {
      lastRenewalStatus_ = "NO_EST_CREDENTIALS";
      break;
    }
    if (cfg.estAuthMode == 2 && cfg.estBootstrapToken.isEmpty()) {
      lastRenewalStatus_ = "NO_EST_BOOTSTRAP_TOKEN";
      break;
    }

    EstClient est;
    String chainPem, newKey;
    String subject = "CN=";
    subject += Config::DEVICE_ID;
    audit("ROTATION_CREDENTIAL_GENERATION_START");
    if (!est.enroll(cfg.estServerUrl, cfg.estLabel, cfg.estAuthMode,
                    cfg.estUsername, cfg.estPassword, cfg.estBootstrapToken,
                    oldCert, oldKey, subject, enrolled_, chainPem, newKey)) {
      ++renewalFailures_;
      lastRenewalStatus_ = "RENEW_FAILED";
      (void)savePersistentState();
      audit("RENEW_FAILED");
      break;
    }
    String selected;
    if (!verifyAndInstall(chainPem, newKey, selected)) {
      ++renewalFailures_;
      lastRenewalStatus_ = "RENEW_FAILED";
      (void)savePersistentState();
      audit("RENEW_FAILED");
      break;
    }
    if (cfg.estAuthMode == 2 && !enrolled_) {
      RuntimeConfig cleared;
      uint32_t generation = 0;
      if (!configSnapshot(cleared, generation)) {
        lastRenewalStatus_ = "TOKEN_CLEAR_FAILED";
        break;
      }
      cleared.estBootstrapToken.clear();
      cleared.estBootstrapTokenConsumed = true;
      if (!configCommit(cleared, generation)) {
        lastRenewalStatus_ = "TOKEN_CLEAR_FAILED";
        audit("TOKEN_CLEAR_FAILED");
        break;
      }
    }
    lastRenewalMs_ = millis();
    renewalFailures_ = 0;
    enrolled_ = true;
    lastRenewalStatus_ = "OK";
    (void)savePersistentState();
    audit("RENEWED");
    ok = true;
  } while (false);
  xSemaphoreGive(mutex_);
  return ok;
}

bool CertLifecycleManager::fetchCaChain() {
  if (!started_ || !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  RuntimeConfig cfg;
  const bool valid = configSnapshot(cfg) && cfg.certLifecycleEnabled &&
                     !cfg.estServerUrl.isEmpty() && clockValid();
  if (!valid) { xSemaphoreGive(mutex_); return false; }
  EstClient est;
  String chain;
  const bool ok = est.fetchCaCerts(cfg.estServerUrl, cfg.estLabel,
                                   mqtt_.clientCertificatePem(), mqtt_.clientPrivateKeyPem(), chain);
  if (ok && storage.ready()) {
    SpiLock lock(pdMS_TO_TICKS(500));
    if (lock.ok()) {
      if (!SD.exists("/CERT")) (void)SD.mkdir("/CERT");
      File f = SD.open("/CERT/EST-CA-CHAIN.PEM", FILE_WRITE);
      if (f) { f.print(chain); f.close(); }
    }
  }
  xSemaphoreGive(mutex_);
  return ok;
}

bool CertLifecycleManager::fetchCsrAttrs() {
  if (!started_ || !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  RuntimeConfig cfg;
  const bool valid = configSnapshot(cfg) && cfg.certLifecycleEnabled &&
                     !cfg.estServerUrl.isEmpty() && clockValid();
  if (!valid) { xSemaphoreGive(mutex_); return false; }
  EstClient est;
  std::vector<uint8_t> attrs;
  const bool ok = est.fetchCsrAttrs(cfg.estServerUrl, cfg.estLabel,
                                    mqtt_.clientCertificatePem(), mqtt_.clientPrivateKeyPem(), attrs);
  if (ok && storage.ready()) {
    SpiLock lock(pdMS_TO_TICKS(500));
    if (lock.ok()) {
      if (!SD.exists("/CERT")) (void)SD.mkdir("/CERT");
      File f = SD.open("/CERT/EST-CSRATTRS.DER", FILE_WRITE);
      if (f) { f.write(attrs.data(), attrs.size()); f.close(); }
    }
  }
  xSemaphoreGive(mutex_);
  return ok;
}

bool CertLifecycleManager::dueForRenewal(uint64_t now) const {
  RuntimeConfig cfg;
  if (!configSnapshot(cfg) || !cfg.certLifecycleEnabled || cfg.certRenewalThresholdDays == 0) return false;
  const uint64_t threshold = static_cast<uint64_t>(cfg.certRenewalThresholdDays) * 86400ULL;
  return expiryEpoch_ == 0 || expiryEpoch_ <= now || expiryEpoch_ - now <= threshold;
}

void CertLifecycleManager::task() {
  if (!started_ || !enabled()) return;
  const uint32_t nowMs = millis();
  if (nextCheckMs_ != 0 && static_cast<int32_t>(nowMs - nextCheckMs_) < 0) return;
  RuntimeConfig cfg;
  if (!configSnapshot(cfg)) return;
  nextCheckMs_ = nowMs + cfg.certCheckPeriodMs;
  if (!clockValid()) {
    lastRenewalStatus_ = "CLOCK_INVALID";
    return;
  }
  uint64_t now = 0;
  {
    StateLock lock(gState);
    if (lock.ok()) now = gState.gps.utcEpoch;
  }
  if (expiryEpoch_ == 0) {
    if (parseCertificate(mqtt_.clientCertificatePem(), true)) audit("VALIDATED");
  }
  if (expiryEpoch_ != 0 && now >= expiryEpoch_) audit("EXPIRED");
  if (dueForRenewal(now)) (void)renewCertificate(false);
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.certLifecycleEnabled = cfg.certLifecycleEnabled;
      gState.certExpiryEpoch = expiryEpoch_;
      gState.certRenewalFailures = renewalFailures_;
      gState.certLifecycleStatus = lastRenewalStatus_;
    }
  }
}

String CertLifecycleManager::jsonEscape(const String& value) const {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (static_cast<uint8_t>(c) < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

String CertLifecycleManager::statusJson() const {
  RuntimeConfig cfg;
  (void)configSnapshot(cfg);
  uint64_t now = 0;
  { StateLock lock(gState); if (lock.ok() && gState.gps.timeValid) now = gState.gps.utcEpoch; }
  int64_t days = 0;
  if (expiryEpoch_ && now) days = expiryEpoch_ >= now ? static_cast<int64_t>((expiryEpoch_ - now) / 86400ULL) : -static_cast<int64_t>((now - expiryEpoch_) / 86400ULL);
  String j = "{\"ok\":true,\"subject\":\"" + jsonEscape(subject_) +
             "\",\"issuer\":\"" + jsonEscape(issuer_) +
             "\",\"serial\":\"" + jsonEscape(serial_) +
             "\",\"notBefore\":" + String(static_cast<unsigned long long>(notBeforeEpoch_)) +
             ",\"notAfter\":" + String(static_cast<unsigned long long>(expiryEpoch_)) +
             ",\"daysRemaining\":" + String(static_cast<long long>(days)) +
             ",\"renewalThresholdDays\":" + String(cfg.certRenewalThresholdDays) +
             ",\"lifecycleEnabled\":" + String(cfg.certLifecycleEnabled ? "true" : "false") +
             ",\"estServerUrl\":\"" + jsonEscape(cfg.estServerUrl) +
             "\",\"estLabel\":\"" + jsonEscape(cfg.estLabel) +
             "\",\"lastRenewalMs\":" + String(lastRenewalMs_) +
             ",\"lastRenewalStatus\":\"" + jsonEscape(lastRenewalStatus_) +
             "\",\"renewalFailures\":" + String(renewalFailures_) + "}";
  return j;
}

String CertLifecycleManager::historyJson() const {
  if (!storage.ready()) return "[]";
  std::vector<String> lines;
  lines.reserve(20);
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return "[]";
  const char* paths[] = {"/LOG/CERT-LIFECYCLE.1.LOG", "/LOG/CERT-LIFECYCLE.LOG"};
  for (const char* path : paths) {
    File f = SD.open(path, FILE_READ);
    if (!f) continue;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (!line.isEmpty()) {
        lines.push_back(line);
        if (lines.size() > 20) lines.erase(lines.begin());
      }
    }
    f.close();
  }
  String out = "[";
  size_t emitted = 0;
  for (auto it = lines.rbegin(); it != lines.rend() && emitted < 10; ++it, ++emitted) {
    if (emitted) out += ',';
    out += '"';
    out += jsonEscape(*it);
    out += '"';
  }
  out += ']';
  return out;
}

String CertLifecycleManager::getSubject() const { return subject_; }
String CertLifecycleManager::getIssuer() const { return issuer_; }
String CertLifecycleManager::getSerial() const { return serial_; }
uint64_t CertLifecycleManager::getExpiryEpoch() const { return expiryEpoch_; }
uint32_t CertLifecycleManager::lastRenewalMs() const { return lastRenewalMs_; }
uint16_t CertLifecycleManager::renewalFailures() const { return renewalFailures_; }

void CertLifecycleManager::audit(const char* event) {
  if (!storage.ready() || !event) return;
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  const char* path = "/LOG/CERT-LIFECYCLE.LOG";
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (f.size() >= Config::CONFIG_AUDIT_LOG_ROTATE_BYTES) {
    f.close();
    const char* old = "/LOG/CERT-LIFECYCLE.1.LOG";
    if (SD.exists(old)) SD.remove(old);
    (void)SD.rename(path, old);
    f = SD.open(path, FILE_APPEND);
  }
  if (f) {
    uint64_t epoch = 0;
    { StateLock lock2(gState); if (lock2.ok()) epoch = gState.gps.utcEpoch; }
    f.printf("%llu,%s,%s,%s,%s,%llu,%llu,%s\n",
             static_cast<unsigned long long>(epoch), Config::DEVICE_ID,
             subject_.c_str(), issuer_.c_str(), serial_.c_str(),
             static_cast<unsigned long long>(notBeforeEpoch_),
             static_cast<unsigned long long>(expiryEpoch_), event);
    f.close();
  }
}
