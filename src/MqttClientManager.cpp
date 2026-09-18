#include "MqttClientManager.h"
#include "Config.h"
#include "MqttCaCert.h"
#include "Telemetry.h"
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {
constexpr char NVS_NS[] = "mqtt_creds";
constexpr time_t MIN_VALID_EPOCH = 1700000000;
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

bool MqttClientManager::loadCredentials() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  host_ = prefs.getString("host", Config::MQTT_HOST);
  port_ = prefs.getUShort("port", Config::MQTT_PORT);
  user_ = prefs.getString("user", Config::MQTT_USERNAME);
  pass_ = prefs.getString("pass", Config::MQTT_PASSWORD);
  prefs.end();
  return !host_.isEmpty() && port_ != 0;
}

bool MqttClientManager::begin() {
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
  useTls_ = port_ != 1883;
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
  useTls_ = port_ != 1883;
  if (useTls_) {
    secure_.setCACert(MQTT_BROKER_ROOT_CA);
    secure_.setHandshakeTimeout(10);
    client_.setClient(secure_);
  } else {
    client_.setClient(plain_);
  }
  client_.setServer(host_.c_str(), port_);
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  const bool saved = prefs.putString("host", host_) > 0 &&
                     prefs.putUShort("port", port_) > 0 &&
                     prefs.putString("user", user_) > 0 &&
                     prefs.putString("pass", pass_) > 0;
  prefs.end();
  connected_ = false;
  nextRetryMs_ = 0;
  return saved;
}

bool MqttClientManager::publish(const String& topic, const String& payload, bool retained) {
  if (!isConnected() || topic.isEmpty() || payload.isEmpty()) return false;
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

void MqttClientManager::task() {
  if (host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
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
          retryDelayMs_ = min<uint32_t>(Config::MQTT_RECONNECT_MAX_MS, retryDelayMs_ * 2U);
          return;
        }
      }
    }
    if (user_.isEmpty()) {
      ok = client_.connect(clientId.c_str(), nullptr, nullptr, willTopic.c_str(),
                           0, Config::MQTT_RETAIN_AVAILABILITY, willPayload, true);
    } else {
      ok = client_.connect(clientId.c_str(), user_.c_str(), pass_.c_str(),
                           willTopic.c_str(), 0, Config::MQTT_RETAIN_AVAILABILITY,
                           willPayload, true);
    }
    if (ok) {
      connected_ = true;
      retryDelayMs_ = Config::MQTT_RECONNECT_MIN_MS;
      publish(willTopic, "online", Config::MQTT_RETAIN_AVAILABILITY);
    } else {
      retryDelayMs_ = min<uint32_t>(Config::MQTT_RECONNECT_MAX_MS, retryDelayMs_ * 2U);
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
  if (connected_ && millis() - lastPublishMs >= Config::MQTT_PUBLISH_PERIOD_MS) {
    const String payload = makeLoRaWANUplinkJson();
    if (!payload.isEmpty() &&
        publish(topic("telemetry"), payload, Config::MQTT_RETAIN_TELEMETRY)) {
      lastPublishMs = millis();
    }
  }
}
