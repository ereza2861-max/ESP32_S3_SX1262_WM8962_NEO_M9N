#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "EstClient.h"
#include <mbedtls/x509_crt.h>

class MqttClientManager;

class CertLifecycleManager {
public:
  explicit CertLifecycleManager(MqttClientManager& mqtt);
  bool begin();
  void task();
  bool renewCertificate(bool manual = false);
  bool fetchCaChain();
  bool fetchCsrAttrs();
  String statusJson() const;
  String historyJson() const;
  bool enabled() const;
  uint64_t getExpiryEpoch() const;
  String getSubject() const;
  String getIssuer() const;
  String getSerial() const;
  uint32_t lastRenewalMs() const;
  uint16_t renewalFailures() const;

private:
  MqttClientManager& mqtt_;
  SemaphoreHandle_t mutex_ = nullptr;
  uint32_t nextCheckMs_ = 0;
  uint32_t lastRenewalMs_ = 0;
  uint16_t renewalFailures_ = 0;
  String lastRenewalStatus_ = "NEVER";
  uint64_t expiryEpoch_ = 0;
  uint64_t notBeforeEpoch_ = 0;
  String subject_;
  String issuer_;
  String serial_;
  bool started_ = false;
  bool enrolled_ = false;

  bool parseCertificate(const String& pem, bool verifyChain);
  bool verifyAndInstall(const String& certChainPem, const String& newKeyPem, String& selectedCertPem);
  bool atomicStore(const String& certPem, const String& keyPem);
  bool loadPersistentState();
  bool savePersistentState();
  void audit(const char* event);
  bool clockValid() const;
  bool dueForRenewal(uint64_t now) const;
  String jsonEscape(const String& value) const;
  static uint64_t timeToEpoch(const mbedtls_x509_time& t);
};
