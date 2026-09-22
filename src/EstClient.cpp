#include "EstClient.h"
#include <HTTPClient.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/base64.h>
#include <esp_random.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include "EstCaCert.h"

namespace {
constexpr size_t MAX_EST_RESPONSE = 64U * 1024U;
constexpr uint32_t EST_TIMEOUT_MS = 15000;

int espRng(void*, unsigned char* out, size_t len) {
  if (!out) return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
  esp_fill_random(out, len);
  return 0;
}

bool derLength(const uint8_t* p, size_t remaining, size_t& header, size_t& length) {
  if (!p || remaining < 2) return false;
  const uint8_t first = p[1];
  if ((first & 0x80U) == 0) {
    header = 2;
    length = first;
    return length <= remaining - header;
  }
  const uint8_t count = first & 0x7FU;
  if (count == 0 || count > 4 || remaining < 2U + count) return false;
  size_t n = 0;
  for (uint8_t i = 0; i < count; ++i) n = (n << 8U) | p[2U + i];
  header = 2U + count;
  length = n;
  return length <= remaining - header;
}

bool collectCerts(const uint8_t* der, size_t len, std::vector<std::vector<uint8_t>>& certs, uint8_t depth = 0) {
  if (!der || len < 2 || depth > 12) return false;
  size_t off = 0;
  while (off + 2 <= len) {
    size_t hdr = 0, body = 0;
    if (!derLength(der + off, len - off, hdr, body)) return false;
    const uint8_t tag = der[off];
    const uint8_t* value = der + off + hdr;
    if (tag == 0x30 && body >= 32 && body <= 16384) {
      mbedtls_x509_crt crt;
      mbedtls_x509_crt_init(&crt);
      const int rc = mbedtls_x509_crt_parse_der(&crt, der + off, hdr + body);
      if (rc == 0 && crt.raw.p && crt.raw.len) {
        certs.emplace_back(crt.raw.p, crt.raw.p + crt.raw.len);
      }
      mbedtls_x509_crt_free(&crt);
    }
    if ((tag & 0x20U) != 0U || tag == 0x30 || tag == 0x31 || (tag & 0xC0U) == 0x80U) {
      if (body > 0 && !collectCerts(value, body, certs, depth + 1)) return false;
    }
    off += hdr + body;
  }
  return off == len;
}
}

bool EstClient::parseHttpsUrl(const String& url, String& host, uint16_t& port, String& basePath) const {
  if (!url.startsWith("https://")) return false;
  String rest = url.substring(8);
  const int slash = rest.indexOf('/');
  String authority = slash >= 0 ? rest.substring(0, slash) : rest;
  basePath = slash >= 0 ? rest.substring(slash) : String("/");
  if (authority.isEmpty() || authority.length() > 253) return false;

  const int colon = authority.lastIndexOf(':');
  if (colon > 0 && authority.indexOf(']') < 0) {
    host = authority.substring(0, colon);
    const String portText = authority.substring(colon + 1);
    if (portText.isEmpty() || portText.length() > 5) return false;
    uint32_t parsed = 0;
    for (char c : portText) {
      if (c < '0' || c > '9') return false;
      parsed = parsed * 10U + static_cast<uint32_t>(c - '0');
      if (parsed > 65535U) return false;
    }
    port = static_cast<uint16_t>(parsed);
  } else {
    host = authority;
    port = 443;
  }
  return !host.isEmpty() && port != 0;
}

String EstClient::endpoint(const String& serverUrl, const String& label, const char* suffix) const {
  String base = serverUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  String path = label;
  if (path.isEmpty()) path = "/.well-known/est";
  if (!path.startsWith("/")) path = "/" + path;
  while (path.endsWith("/")) path.remove(path.length() - 1);
  return base + path + "/" + suffix;
}

bool EstClient::request(const String& method, const String& url,
                        uint8_t authMode, const String& username, const String& password,
                        const String& bootstrapToken, const String& clientCertPem,
                        const String& clientKeyPem, const String& body,
                        const char* contentType, std::vector<uint8_t>& response,
                        int& statusCode) {
  response.clear();
  String host, basePath;
  uint16_t port = 0;
  if (!parseHttpsUrl(url, host, port, basePath) || authMode > 2) return false;

  WiFiClientSecure tls;
  tls.setCACert(estTrustAnchor());
  if (authMode == 0) {
    if (clientCertPem.isEmpty() || clientKeyPem.isEmpty()) return false;
    tls.setCertificate(clientCertPem.c_str());
    tls.setPrivateKey(clientKeyPem.c_str());
  } else if (authMode == 1) {
    if (username.isEmpty() || password.isEmpty()) return false;
  } else {
    if (bootstrapToken.isEmpty()) return false;
  }
  tls.setHandshakeTimeout(10);

  HTTPClient http;
  http.setConnectTimeout(EST_TIMEOUT_MS);
  http.setTimeout(EST_TIMEOUT_MS);
  if (!http.begin(tls, url)) return false;
  http.addHeader("Accept", "application/pkcs7-mime, application/pkcs10, application/csrattrs");
  if (contentType) http.addHeader("Content-Type", contentType);
  http.addHeader("Cache-Control", "no-store");

  if (authMode == 1) {
    const String credentials = username + ":" + password;
    const size_t encodedCap = 4U * ((credentials.length() + 2U) / 3U) + 1U;
    std::vector<unsigned char> encoded(encodedCap);
    size_t encodedLen = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &encodedLen,
                              reinterpret_cast<const unsigned char*>(credentials.c_str()),
                              credentials.length()) != 0) {
      http.end();
      return false;
    }
    encoded[encodedLen] = '\0';
    http.addHeader("Authorization", "Basic " + String(reinterpret_cast<const char*>(encoded.data())));
  } else if (authMode == 2) {
    http.addHeader("Authorization", "Bearer " + bootstrapToken);
  }

  int code = -1;
  if (method == "POST") code = http.POST(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
  else if (method == "GET") code = http.GET();
  else { http.end(); return false; }
  statusCode = code;
  if (code <= 0) { http.end(); return false; }

  NetworkClient* stream = http.getStreamPtr();
  const int reported = http.getSize();
  size_t reserveSize = reported > 0 ? static_cast<size_t>(reported) : 4096U;
  if (reserveSize > MAX_EST_RESPONSE) { http.end(); return false; }
  response.reserve(reserveSize);
  const uint32_t started = millis();
  while (http.connected() && (reported < 0 || response.size() < static_cast<size_t>(reported))) {
    while (stream->available()) {
      const size_t remaining = MAX_EST_RESPONSE - response.size();
      if (!remaining) { http.end(); return false; }
      uint8_t buf[1024];
      const int want = static_cast<int>(std::min<size_t>(sizeof(buf), remaining));
      const int got = stream->readBytes(reinterpret_cast<char*>(buf), want);
      if (got <= 0) break;
      response.insert(response.end(), buf, buf + got);
    }
    if (reported > 0 && response.size() >= static_cast<size_t>(reported)) break;
    if (millis() - started > EST_TIMEOUT_MS) { http.end(); return false; }
    delay(1);
  }
  http.end();
  return true;
}

bool EstClient::generateKeyAndCsr(const String& subject, String& keyPem, String& csrPem) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  mbedtls_x509write_csr csr;
  mbedtls_x509write_csr_init(&csr);
  bool ok = false;
  uint8_t keyBuf[4096] = {};
  uint8_t csrBuf[4096] = {};

  do {
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) break;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), espRng, nullptr) != 0) break;
    int n = mbedtls_pk_write_key_pem(&pk, keyBuf, sizeof(keyBuf));
    if (n != 0) break;

    mbedtls_x509write_csr_set_key(&csr, &pk);
    if (mbedtls_x509write_csr_set_subject_name(&csr, subject.c_str()) != 0) break;
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    n = mbedtls_x509write_csr_pem(&csr, csrBuf, sizeof(csrBuf), espRng, nullptr);
    if (n != 0) break;
    keyPem = reinterpret_cast<const char*>(keyBuf);
    csrPem = reinterpret_cast<const char*>(csrBuf);
    ok = !keyPem.isEmpty() && !csrPem.isEmpty();
  } while (false);

  mbedtls_x509write_csr_free(&csr);
  mbedtls_pk_free(&pk);
  return ok;
}

bool EstClient::pemEncode(const uint8_t* der, size_t len, String& pem) const {
  if (!der || len == 0 || len > 16384) return false;
  size_t outLen = 0;
  const size_t b64Len = 4U * ((len + 2U) / 3U);
  std::vector<unsigned char> b64(b64Len + 1U);
  if (mbedtls_base64_encode(b64.data(), b64.size(), &outLen, der, len) != 0) return false;
  pem = "-----BEGIN CERTIFICATE-----\n";
  for (size_t i = 0; i < outLen; i += 64) {
    const size_t chunk = std::min<size_t>(64, outLen - i);
    pem.concat(reinterpret_cast<const char*>(b64.data() + i), chunk);
    pem += '\n';
  }
  pem += "-----END CERTIFICATE-----\n";
  return true;
}

bool EstClient::extractCertificates(const std::vector<uint8_t>& der, String& pemChain) const {
  std::vector<std::vector<uint8_t>> certs;
  if (der.empty() || der.size() > MAX_EST_RESPONSE ||
      !collectCerts(der.data(), der.size(), certs) || certs.empty()) return false;
  pemChain.clear();
  for (const auto& cert : certs) {
    String pem;
    if (!pemEncode(cert.data(), cert.size(), pem)) return false;
    if (pemChain.length() + pem.length() > 32768) return false;
    pemChain += pem;
  }
  return !pemChain.isEmpty();
}

bool EstClient::enroll(const String& serverUrl, const String& label,
                       uint8_t authMode, const String& username, const String& password,
                       const String& bootstrapToken, const String& clientCertPem,
                       const String& clientKeyPem, const String& subject, bool renewal,
                       String& newCertificatePem, String& newPrivateKeyPem) {
  String keyPem, csrPem;
  if (!generateKeyAndCsr(subject, keyPem, csrPem)) return false;
  const String url = endpoint(serverUrl, label, renewal ? "simplereenroll" : "simpleenroll");
  std::vector<uint8_t> response;
  int status = 0;
  if (!request("POST", url, authMode, username, password, bootstrapToken,
               clientCertPem, clientKeyPem, csrPem, "application/pkcs10", response, status) ||
      status < 200 || status >= 300) return false;
  String chain;
  if (!extractCertificates(response, chain)) return false;
  newCertificatePem = chain;
  newPrivateKeyPem = keyPem;
  return true;
}

bool EstClient::fetchCaCerts(const String& serverUrl, const String& label,
                             uint8_t authMode, const String& username, const String& password,
                             const String& bootstrapToken, const String& clientCertPem,
                             const String& clientKeyPem, String& caChainPem) {
  std::vector<uint8_t> response;
  int status = 0;
  if (!request("GET", endpoint(serverUrl, label, "cacerts"),
               authMode, username, password, bootstrapToken, clientCertPem, clientKeyPem,
               String(), nullptr, response, status) ||
      status < 200 || status >= 300) return false;
  return extractCertificates(response, caChainPem);
}

bool EstClient::fetchCsrAttrs(const String& serverUrl, const String& label,
                              uint8_t authMode, const String& username, const String& password,
                              const String& bootstrapToken, const String& clientCertPem,
                              const String& clientKeyPem, std::vector<uint8_t>& attrs) {
  int status = 0;
  return request("GET", endpoint(serverUrl, label, "csrattrs"),
                 authMode, username, password, bootstrapToken, clientCertPem, clientKeyPem,
                 String(), "application/csrattrs", attrs, status) && status >= 200 && status < 300;
}
