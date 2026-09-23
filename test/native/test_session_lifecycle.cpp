#include <cassert>
#include <cstdint>
#include <cstring>
#include <openssl/hmac.h>
#include "WebUiSessionPolicy.h"

int main() {
  const uint8_t secret[32] = {};
  const uint32_t issued = 123456U;
  const uint32_t ipA = 0x0100007FU;  // 127.0.0.1 in the firmware's packed form
  const uint32_t ipB = 0x0200007FU;

  uint8_t msgA[8] = {};
  uint8_t msgB[8] = {};
  WebUiSessionPolicy::makeSessionMessage(ipA, issued, msgA);
  WebUiSessionPolicy::makeSessionMessage(ipB, issued, msgB);
  assert(std::memcmp(msgA, msgB, sizeof(msgA)) != 0);

  unsigned int digestLen = 0;
  uint8_t tokenA[32] = {};
  uint8_t tokenB[32] = {};
  assert(HMAC(EVP_sha256(), secret, sizeof(secret), msgA, sizeof(msgA),
              tokenA, &digestLen) != nullptr);
  assert(digestLen == 32U);
  unsigned int digestLenB = 0;
  assert(HMAC(EVP_sha256(), secret, sizeof(secret), msgB, sizeof(msgB),
              tokenB, &digestLenB) != nullptr);
  assert(digestLenB == 32U);
  assert(std::memcmp(tokenA, tokenB, sizeof(tokenA)) != 0);

  char tokenHex[65] = {};
  static const char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    tokenHex[i * 2] = digits[tokenA[i] >> 4];
    tokenHex[i * 2 + 1] = digits[tokenA[i] & 0x0F];
  }
  assert(WebUiSessionPolicy::isHexToken(
      tokenHex, 64, WebUiSessionPolicy::SESSION_TOKEN_HEX_LENGTH));
  assert(!WebUiSessionPolicy::isHexToken(
      tokenHex, 63, WebUiSessionPolicy::SESSION_TOKEN_HEX_LENGTH));

  assert(WebUiSessionPolicy::isFresh(issued + 999, issued, 1000));
  assert(!WebUiSessionPolicy::isFresh(issued + 1000, issued, 1000));

  char cookie[160] = {};
  assert(WebUiSessionPolicy::buildSessionCookie(tokenHex, 900, cookie, sizeof(cookie)));
  const char* expected =
      "Path=/; HttpOnly; Secure; SameSite=Strict";
  assert(std::strstr(cookie, expected) != nullptr);
  assert(std::strstr(cookie, "Max-Age=900") != nullptr);

  const char csrf[] = "0123456789abcdef0123456789abcdef";
  assert(WebUiSessionPolicy::isHexToken(
      csrf, 32, WebUiSessionPolicy::CSRF_TOKEN_HEX_LENGTH));
  return 0;
}
