#include <cassert>
#include <cstdint>
#include <cstring>
#include <openssl/evp.h>

static void testPbkdf2Sha256Vector() {
  const char password[] = "password";
  const uint8_t salt[] = {'s','a','l','t'};
  const uint8_t expected[32] = {
      0x03,0x94,0xa2,0xed,0xe3,0x32,0xc9,0xa1,
      0x3e,0xb8,0x2e,0x9b,0x24,0x63,0x16,0x04,
      0xc3,0x1d,0xf9,0x78,0xb4,0xe2,0xf0,0xfb,
      0xd2,0xc5,0x49,0x94,0x4f,0x9d,0x79,0xa5};
  uint8_t derived[32] = {};
  assert(PKCS5_PBKDF2_HMAC(password, -1, salt, sizeof(salt), 100000,
                           EVP_sha256(), sizeof(derived), derived) == 1);
  assert(std::memcmp(derived, expected, sizeof(expected)) == 0);
}

void setup() { testPbkdf2Sha256Vector(); }
void loop() {}
