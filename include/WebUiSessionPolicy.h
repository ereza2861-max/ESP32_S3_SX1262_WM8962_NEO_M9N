#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace WebUiSessionPolicy {
constexpr std::size_t SESSION_TOKEN_BYTES = 32;
constexpr std::size_t SESSION_TOKEN_HEX_LENGTH = SESSION_TOKEN_BYTES * 2;
constexpr std::size_t CSRF_TOKEN_HEX_LENGTH = 32;

inline void makeSessionMessage(uint32_t ipValue, uint32_t issuedMs,
                               uint8_t out[sizeof(uint32_t) * 2]) {
  std::memcpy(out, &ipValue, sizeof(ipValue));
  std::memcpy(out + sizeof(ipValue), &issuedMs, sizeof(issuedMs));
}

inline bool isFresh(uint32_t nowMs, uint32_t issuedMs, uint32_t timeoutMs) {
  return static_cast<uint32_t>(nowMs - issuedMs) < timeoutMs;
}

inline bool isHexToken(const char* value, std::size_t length,
                       std::size_t expectedLength) {
  if (!value || length != expectedLength) return false;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

inline bool buildSessionCookie(const char* tokenHex, uint32_t maxAgeSec,
                               char* out, std::size_t capacity) {
  if (!isHexToken(tokenHex, tokenHex ? std::strlen(tokenHex) : 0,
                  SESSION_TOKEN_HEX_LENGTH) || !out || capacity == 0)
    return false;
  const int written = std::snprintf(
      out, capacity,
      "FR-SESSION=%s; Max-Age=%lu; Path=/; HttpOnly; Secure; SameSite=Strict",
      tokenHex, static_cast<unsigned long>(maxAgeSec));
  return written >= 0 && static_cast<std::size_t>(written) < capacity;
}
}  // namespace WebUiSessionPolicy
