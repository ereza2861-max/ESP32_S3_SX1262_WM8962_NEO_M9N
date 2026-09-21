#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class MqttClientManager {
public:
  bool begin();
  bool connect(const String& host, uint16_t port,
               const String& user, const String& pass);
  bool provisionCredentials(const String& host, uint16_t port,
                            const String& user, const String& pass);
  bool provisionCertificate(const String& host, uint16_t port,
                            const String& certificatePem, const String& privateKeyPem);
  bool credentialsProvisioned() const { return credentialsProvisioned_; }
  bool passwordRotationWarning() const;
  void setEnabled(bool enabled);
  bool applyConfig();
  bool enabled() const { return enabled_; }
  bool publish(const String& topic, const String& payload, bool retained = false);
  bool publishSensorData(uint32_t nodeId, const char* nodeName,
                         uint16_t sensorId, const char* sensorName,
                         const char* unit, float value, uint8_t quality,
                         int16_t rssi, uint64_t timestampMs);
  void task();
  bool isConnected() const { return connected_ && client_.connected(); }

private:
  struct SensorSample {
    uint32_t nodeId = 0;
    uint16_t sensorId = 0;
    float value = 0.0f;
    uint8_t quality = 0;
    int16_t rssi = -127;
    uint64_t timestampMs = 0;
    char nodeName[25] = {};
    char sensorName[25] = {};
    char unit[13] = {};
  };
  static_assert(sizeof(SensorSample) <= 128, "SensorSample queue item exceeds 128 bytes");

  WiFiClient plain_;
  WiFiClientSecure secure_;
  PubSubClient client_;
  bool useTls_ = false;
  bool connected_ = false;
  bool enabled_ = true;
  String host_;
  String user_;
  String pass_;
  QueueHandle_t sensorQueue_ = nullptr;
  StaticQueue_t sensorQueueStruct_{};
  uint8_t sensorQueueStorage_[16 * sizeof(SensorSample)]{};
  uint16_t port_ = 1883;
  uint32_t nextRetryMs_ = 0;
  uint32_t retryDelayMs_ = 5000;
  bool loadCredentials();
  bool saveCredentials();
  bool encryptCredentials(String& envelope) const;
  bool decryptCredentials(const String& envelope);
  void auditEvent(const char* event, int mqttState = 0);
  bool credentialsProvisioned_ = false;
  bool pkiProvisioned_ = false;
  String clientCertificatePem_;
  String clientPrivateKeyPem_;
  time_t passwordProvisionedEpoch_ = 0;
  bool timeSynchronized() const;
  String topic(const char* leaf) const;
  bool publishSensorSample(const SensorSample& sample);
  bool publishSensorSampleQos1(const String& mqttTopic, const String& payload);
  Client& mqttTransport();
  bool waitForPubAck(uint16_t packetId, uint32_t timeoutMs);
  static size_t encodeMqttRemainingLength(uint8_t* out, size_t length);
  uint16_t nextPacketId_ = 1;
};
