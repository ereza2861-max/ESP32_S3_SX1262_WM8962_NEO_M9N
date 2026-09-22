#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <vector>

class EstClient {
public:
  bool enroll(const String& serverUrl, const String& label,
              uint8_t authMode, const String& username, const String& password,
              const String& bootstrapToken, const String& clientCertPem,
              const String& clientKeyPem, const String& subject, bool renewal,
              String& newCertificatePem, String& newPrivateKeyPem);
  bool fetchCaCerts(const String& serverUrl, const String& label,
                   uint8_t authMode, const String& username, const String& password,
                   const String& bootstrapToken, const String& clientCertPem,
                   const String& clientKeyPem, String& caChainPem);
  bool fetchCsrAttrs(const String& serverUrl, const String& label,
                     uint8_t authMode, const String& username, const String& password,
                     const String& bootstrapToken, const String& clientCertPem,
                     const String& clientKeyPem, std::vector<uint8_t>& attrs);

private:
  bool parseHttpsUrl(const String& url, String& host, uint16_t& port, String& basePath) const;
  bool request(const String& method, const String& url,
               uint8_t authMode, const String& username, const String& password,
               const String& bootstrapToken, const String& clientCertPem,
               const String& clientKeyPem, const String& body,
               const char* contentType, std::vector<uint8_t>& response,
               int& statusCode);
  bool generateKeyAndCsr(const String& subject, String& keyPem, String& csrPem);
  bool extractCertificates(const std::vector<uint8_t>& der, String& pemChain) const;
  bool pemEncode(const uint8_t* der, size_t len, String& pem) const;
  String endpoint(const String& serverUrl, const String& label, const char* suffix) const;
};
