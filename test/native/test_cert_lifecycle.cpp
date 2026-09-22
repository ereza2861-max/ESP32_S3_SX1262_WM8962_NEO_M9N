#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <string>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace {
std::string run(const char* cmd) {
  FILE* p = popen(cmd, "r");
  assert(p);
  std::string out;
  char buf[256];
  while (fgets(buf, sizeof(buf), p)) out += buf;
  const int rc = pclose(p);
  assert(rc == 0);
  return out;
}

bool expiryWithin(const time_t now, const time_t expiry, unsigned thresholdDays) {
  if (expiry <= now) return true;
  return static_cast<unsigned long long>(expiry - now) <=
         static_cast<unsigned long long>(thresholdDays) * 86400ULL;
}
}

int main() {
  char dirTemplate[] = "/tmp/fieldradio-cert-test-XXXXXX";
  char* dir = mkdtemp(dirTemplate);
  assert(dir);
  std::string base(dir);
  std::string key = base + "/key.pem";
  std::string csr = base + "/req.pem";
  std::string cert = base + "/cert.pem";

  std::string cmd = "openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out " + key;
  run(cmd.c_str());
  cmd = "openssl req -new -key " + key + " -subj /CN=ESP32S3_VOICE_NODE_01 -out " + csr;
  run(cmd.c_str());
  cmd = "openssl req -x509 -new -key " + key +
        " -sha256 -days 30 -subj /CN=ESP32S3_VOICE_NODE_01 -out " + cert;
  run(cmd.c_str());

  FILE* f = fopen(cert.c_str(), "r");
  assert(f);
  X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr);
  fclose(f);
  assert(x);
  const ASN1_TIME* notAfter = X509_get0_notAfter(x);
  int days = 0, seconds = 0;
  assert(ASN1_TIME_diff(&days, &seconds, nullptr, notAfter) == 1);
  assert(days >= 29 && days <= 30);

  const time_t now = std::time(nullptr);
  const time_t expiry = now + 30 * 86400;
  assert(!expiryWithin(now, expiry, 7));
  assert(expiryWithin(now, expiry, 30));
  assert(expiryWithin(now + 31 * 86400, expiry, 30));

  EVP_PKEY* pkey = nullptr;
  EVP_PKEY_CTX* keygen = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  assert(keygen);
  assert(EVP_PKEY_keygen_init(keygen) == 1);
  assert(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keygen, NID_X9_62_prime256v1) == 1);
  assert(EVP_PKEY_keygen(keygen, &pkey) == 1);
  X509_REQ* req = X509_REQ_new();
  assert(req);
  X509_NAME* name = X509_NAME_new();
  assert(name);
  assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    reinterpret_cast<const unsigned char*>("ESP32S3_VOICE_NODE_01"),
                                    -1, -1, 0) == 1);
  assert(X509_REQ_set_subject_name(req, name) == 1);
  assert(X509_REQ_set_pubkey(req, pkey) == 1);
  assert(X509_REQ_sign(req, pkey, EVP_sha256()) > 0);
  EVP_PKEY* reqPub = X509_REQ_get0_pubkey(req);
  assert(reqPub != nullptr);

  X509_NAME_free(name);
  X509_REQ_free(req);
  EVP_PKEY_free(pkey);
  EVP_PKEY_CTX_free(keygen);
  X509_free(x);

  std::string rm = "rm -rf " + base;
  std::system(rm.c_str());
  return 0;
}
