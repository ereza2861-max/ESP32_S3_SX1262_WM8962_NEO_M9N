#include <cassert>
#include <cstdint>
#include <cstring>
#include <openssl/hmac.h>

namespace {
void makeHmac(const uint8_t* data, size_t len, uint8_t out[16]) {
  const uint8_t key[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                           0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
  unsigned int n = 0;
  uint8_t full[32] = {};
  HMAC(EVP_sha256(), key, sizeof(key), data, len, full, &n);
  std::memcpy(out, full, 16);
}
void test_byte_change_is_detected() {
  uint8_t record[32] = {};
  record[0] = 0x53; record[7] = 0x42;
  uint8_t a[16] = {}, b[16] = {};
  makeHmac(record, sizeof(record), a);
  record[7] ^= 0x01;
  makeHmac(record, sizeof(record), b);
  assert(std::memcmp(a, b, sizeof(a)) != 0);
}
}  // namespace

int main() {
  test_byte_change_is_detected();
  return 0;
}
