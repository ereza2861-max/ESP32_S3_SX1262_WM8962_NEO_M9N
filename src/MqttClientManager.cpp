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
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  credentialsProvisioned_ = prefs.getBool("provisioned", false);
  const String blob = prefs.getString("blob", "");
  passwordProvisionedEpoch_ = static_cast<time_t>(prefs.getLong64("pass_epoch", 0));
  prefs.end();

#if defined(FIELDRADIO_PRODUCTION_BUILD) || (CONFIG_SECURE_BOOT_V2_ENABLED && CONFIG_SECURE_FLASH_ENC_ENABLED)
  if (!credentialsProvisioned_ || blob.isEmpty()) return false;
#else
  if (!credentialsProvisioned_ || blob.isEmpty()) {
    host_ = gConfig.mqttHost;
    port_ = gConfig.mqttPort;
    user_ = Config::MQTT_USERNAME;
    pass_ = Config::MQTT_PASSWORD;
    return !host_.isEmpty() && port_ != 0;
  }
#endif
  if (!decryptCredentials(blob)) return false;
  // Credentials and endpoint are separate runtime concerns. The encrypted
  // credential blob remains the source of username/password, while endpoint
  // policy is taken from the validated RuntimeConfig snapshot.
  host_ = gConfig.mqttHost;
  port_ = gConfig.mqttPort;
#if defined(FIELDRADIO_PRODUCTION_BUILD) || (CONFIG_SECURE_BOOT_V2_ENABLED && CONFIG_SECURE_FLASH_ENC_ENABLED)
  if (!gConfig.mqttTlsRequired) return false;
#endif
  return !host_.isEmpty() && port_ != 0;
}

bool MqttClientManager::saveCredentials() {
  String blob;
  if (!encryptCredentials(blob)) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  const bool ok = prefs.putString("blob", blob) > 0 &&
                  prefs.putBool("provisioned", true) &&
                  prefs.putLong64("pass_epoch", static_cast<int64_t>(timeSynchronized() ? time(nullptr) : 0)) > 0;
  prefs.end();
  if (ok) {
    credentialsProvisioned_ = true;
    passwordProvisionedEpoch_ = timeSynchronized() ? time(nullptr) : 0;
  }
  return ok;
}

bool MqttClientManager::provisionCredentials(const String& host, uint16_t port,
                                             const String& user, const String& pass) {
  if (host.isEmpty() || host.length() > 253 || port == 0 ||
      user.length() > 128 || pass.length() > 128 ||
      host.indexOf('|') >= 0 || user.indexOf('|') >= 0 || pass.indexOf('|') >= 0) return false;
#if defined(FIELDRADIO_PRODUCTION_BUILD) || (CONFIG_SECURE_BOOT_V2_ENABLED && CONFIG_SECURE_FLASH_ENC_ENABLED)
  if (port == 1883) return false;
#endif
  host_ = host; port_ = port; user_ = user; pass_ = pass;
  if (!saveCredentials()) return false;

  plain_.stop();
  secure_.stop();
  useTls_ = gConfig.mqttTlsRequired;
#if defined(FIELDRADIO_PRODUCTION_BUILD) || (CONFIG_SECURE_BOOT_V2_ENABLED && CONFIG_SECURE_FLASH_ENC_ENABLED)
  if (!gConfig.mqttTlsRequired) return false;
#endif
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
    secure_.setHandshakeTimeout(10);
    client_.setClient(secure_);
  } else {
    client_.setClient(plain_);
  }
  client_.setServer(host_.c_str(), port_);
  connected_ = false;
  nextRetryMs_ = 0;
  retryDelayMs_ = gConfig.mqttReconnectMinMs;
  auditEvent("PROVISIONED");
  return true;
}

bool MqttClientManager::passwordRotationWarning() const {
  if (!credentialsProvisioned_) return false;
  if (passwordProvisionedEpoch_ <= 0) return true;
  const time_t now = time(nullptr);
  return now >= MIN_VALID_EPOCH &&
         now - passwordProvisionedEpoch_ >=
             static_cast<time_t>(gConfig.mqttCredentialRotationDays) * 24LL * 60LL * 60LL;
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
  enabled_ = gConfig.mqttEnabled;
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
  useTls_ = gConfig.mqttTlsRequired;
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
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
  if (host.isEmpty() || host.length() > 253 || port == 0 ||
      user.length() > 128 || pass.length() > 128) return false;
  host_ = host; port_ = port; user_ = user; pass_ = pass;
  plain_.stop();
  secure_.stop();
  useTls_ = gConfig.mqttTlsRequired;
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
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
  return publish(topic(sensorLeaf.c_str()), payload, false);
}

void MqttClientManager::setEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  if (!enabled_) {
    connected_ = false;
    client_.disconnect();
    secure_.stop();
    plain_.stop();
    return;
  }
  nextRetryMs_ = 0;
  retryDelayMs_ = gConfig.mqttReconnectMinMs;
  if (host_.isEmpty()) (void)begin();
}

void MqttClientManager::task() {
  if (!enabled_) return;
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

  if (!client_.connected()) {
    if (connected_) auditEvent("DISCONNECT", client_.state());
    connected_ = false;
    if (millis() - nextRetryMs_ < retryDelayMs_) return;
    nextRetryMs_ = millis();

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
          retryDelayMs_ = min<uint32_t>(gConfig.mqttReconnectMaxMs, retryDelayMs_ * 2U);
          return;
        }
      }
    }
    if (user_.isEmpty()) {
      ok = client_.connect(clientId.c_str(), nullptr, nullptr, willTopic.c_str(),
                           0, gConfig.mqttRetainAvailability, willPayload, true);
    } else {
      ok = client_.connect(clientId.c_str(), user_.c_str(), pass_.c_str(),
                           willTopic.c_str(), 0, gConfig.mqttRetainAvailability,
                           willPayload, true);
    }
    if (ok) {
      connected_ = true;
      auditEvent("CONNECT_OK", client_.state());
      retryDelayMs_ = gConfig.mqttReconnectMinMs;
      publish(willTopic, "online", gConfig.mqttRetainAvailability);
    } else {
      if (client_.state() == MQTT_CONNECT_BAD_CREDENTIALS) auditEvent("AUTH_FAIL", client_.state());
      else auditEvent("CONNECT_FAIL", client_.state());
      retryDelayMs_ = min<uint32_t>(gConfig.mqttReconnectMaxMs, retryDelayMs_ * 2U);
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
  if (connected_ && millis() - lastPublishMs >= gConfig.mqttTelemetryPeriodMs) {
    const String payload = makeLoRaWANUplinkJson();
    if (!payload.isEmpty() &&
        publish(topic("telemetry"), payload, gConfig.mqttRetainTelemetry)) {
      lastPublishMs = millis();
    }
  }

  static uint32_t lastHealthMs = 0;
  if (connected_ && millis() - lastHealthMs >= gConfig.mqttHealthPeriodMs) {
    const String health = String("{\"uptime_ms\":") + String(millis()) +
                          ",\"free_heap\":" + String(ESP.getFreeHeap()) +
                          ",\"mqtt_connected\":true}";
    if (publish(topic("health"), health, gConfig.mqttRetainAvailability))
      lastHealthMs = millis();
  }
}
