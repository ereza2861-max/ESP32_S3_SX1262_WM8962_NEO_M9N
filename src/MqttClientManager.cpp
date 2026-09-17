#include "MqttClientManager.h"
#include "Config.h"
#include "MqttCaCert.h"
#include "Telemetry.h"
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

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
  client_.setKeepAlive(30);
  client_.setSocketTimeout(2);
  net_.setCACert(MQTT_BROKER_ROOT_CA);
  net_.setHandshakeTimeout(10);
  if (!loadCredentials()) return false;
  client_.setServer(host_.c_str(), port_);
  return true;
}

bool MqttClientManager::connect(const String& host, uint16_t port,
                                const String& user, const String& pass) {
  if (host.isEmpty() || host.length() > 253 || port == 0 ||
      user.length() > 128 || pass.length() > 128) return false;
  host_ = host; port_ = port; user_ = user; pass_ = pass;
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
  static uint32_t lastPublishMs = 0;
  if (connected_ && millis() - lastPublishMs >= Config::MQTT_PUBLISH_PERIOD_MS) {
    const String payload = makeLoRaWANUplinkJson();
    if (!payload.isEmpty() &&
        publish(topic("telemetry"), payload, Config::MQTT_RETAIN_TELEMETRY)) {
      lastPublishMs = millis();
    }
  }
}
