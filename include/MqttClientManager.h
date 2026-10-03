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
  uint64_t getCertExpiryEpoch() const { return certExpiryEpoch_; }
  String getCertSubject() const { return certSubject_; }
  String getCertIssuer() const { return certIssuer_; }
  String getCertSerial() const { return certSerial_; }
  String clientCertificatePem() const { return clientCertificatePem_; }
  String clientPrivateKeyPem() const { return clientPrivateKeyPem_; }
  bool reloadCertificateMaterial();
  // Atomically replace the active MQTT credential and defer old-credential retirement
  // until the first successful connection using the new certificate.
  bool replaceConnectionCredentials(const String& previousSerial);
  void setEnabled(bool enabled);
  bool applyConfig();
  bool applyConfig(const RuntimeConfig& config);
  bool enabled() const { return enabled_; }
  bool publish(const String& topic, const String& payload, bool retained = false);
  bool publishSensorData(uint32_t nodeId, const char* nodeName,
                         uint16_t sensorId, const char* sensorName,
                         const char* unit, float value, uint8_t quality,
                         int16_t rssi, uint64_t timestampMs,
                          uint32_t sourceSequence = 0,
                          uint8_t schemaVersion = 0,
                          uint32_t firmwareVersion = 0,
                          uint32_t originNodeId);
  // F1-TODO-3: Service certificate rotation readiness without contacting a backend.
  void serviceRotation();
  // F1-TODO-6: Verify a candidate certificate/CA pair without persisting it.
  bool verifyNewCertificate(const String& certPem,
                            const String& caPem,
                            const String& expectedSerial,
                            uint64_t expectedExpiresAt);
  void task();
  bool isConnected() const { return connected_ && client_.connected(); }

private:
  // F1-TODO-4: Phase model reserved for the firmware-side rotation state machine.
  enum class RotationPhase : uint8_t {
    Idle = 0,
    Requesting = 1,
    Verifying = 2,
    Installing = 3,
    Reconnecting = 4,
    Failed = 5,
  };

  // F1-TODO-2: RAM-only rotation state; no persisted-config fields are added.
  struct RotationState {
    String currentSerial;
    uint64_t expiresAt = 0;
    bool rotationPending = false;
    String pendingSerial;
    uint32_t lastCheckMs = 0;
    uint32_t lastAttemptMs = 0;
    uint8_t retryCount = 0;
    uint8_t phase = 0;
  };
  RotationState rotation_{};
  static constexpr uint32_t ROTATION_CHECK_INTERVAL_MS = 3600000UL;
  static constexpr uint8_t ROTATION_MAX_RETRY = 3;
  static const char* rotationPhaseName(uint8_t phase);

  // F1-TODO-7/9: Journaled certificate installation helpers; used only synchronously.
  bool installPendingCertificate(const String& certPem,
                                 const String& keyPem,
                                 const String& caPem,
                                 const String& serial);
  bool rollbackPendingInstall();

  struct SensorSample {
    uint32_t nodeId = 0;
    uint16_t sensorId = 0;
    float value = 0.0f;
    uint8_t quality = 0;
    int16_t rssi = -127;
    uint64_t timestampMs = 0;
    uint32_t sourceSequence = 0;
    uint8_t schemaVersion = 0;
    uint32_t firmwareVersion = 0;
    uint32_t originNodeId = 0;
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
  uint64_t certExpiryEpoch_ = 0;
  String certSubject_;
  String certIssuer_;
  String certSerial_;
  bool timeSynchronized() const;
  String topic(const char* leaf) const;
  bool publishSensorSample(const SensorSample& sample);
  bool publishSensorSampleQos1(const String& mqttTopic, const String& payload, uint32_t sampleId);
  Client& mqttTransport();
  bool waitForPubAck(uint16_t packetId, uint32_t timeoutMs);
  static size_t encodeMqttRemainingLength(uint8_t* out, size_t length);
  uint16_t nextPacketId_ = 1;
  String rotationPreviousSerial_;
  bool rotationRetirementPending_ = false;
  bool retirePreviousCredential();
};
