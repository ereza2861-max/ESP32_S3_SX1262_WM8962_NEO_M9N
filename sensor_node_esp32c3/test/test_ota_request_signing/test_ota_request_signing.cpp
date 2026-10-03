#include <cassert>
#include <cstdint>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/evp.h>

static void testHmacNonceBodyVector() {
  uint8_t key[32] = {};
  uint8_t nonce[16] = {};
  for (uint8_t i = 0; i < 32; ++i) key[i] = i;
  for (uint8_t i = 0; i < 16; ++i) nonce[i] = static_cast<uint8_t>(0x10 + i);
  const uint8_t body[] = "firmware";
  const uint8_t expected[32] = {
      0x41,0xc9,0xa5,0xce,0xf9,0xfa,0x04,0xe3,
      0x87,0x03,0x29,0xc6,0xaf,0x6d,0x3c,0xcf,
      0x72,0x21,0xf1,0x5d,0xd0,0x1a,0xe4,0x68,
      0x06,0x97,0xb2,0xed,0xb6,0x0a,0x36,0x5f};
  uint8_t input[24] = {};
  std::memcpy(input, nonce, sizeof(nonce));
  std::memcpy(input + sizeof(nonce), body, sizeof(body) - 1);
  unsigned int outLen = 0;
  uint8_t actual[32] = {};
  assert(HMAC(EVP_sha256(), key, sizeof(key), input, sizeof(input),
              actual, &outLen) != nullptr);
  assert(outLen == sizeof(actual));
  assert(std::memcmp(actual, expected, sizeof(expected)) == 0);
}

void setup() { testHmacNonceBodyVector(); }
void loop() {}
