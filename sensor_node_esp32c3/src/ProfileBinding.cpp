#include "ProfileBinding.h"

#include <cstdio>
#include <cstring>

#if defined(ARDUINO)
#include <mbedtls/sha256.h>
#else
#include <openssl/sha.h>
#endif

#ifndef FIELD_RADIO_PROFILE_SKU
#define FIELD_RADIO_PROFILE_SKU 0
#endif

namespace ProfileBinding {

Tag firmwareProfileHash() {
  char material[96] = {};
  std::snprintf(material, sizeof(material),
                "FieldRadio|profile-sku|%u|binding-v1",
                static_cast<unsigned>(FIELD_RADIO_PROFILE_SKU));
  Tag tag{};
#if defined(ARDUINO)
  (void)mbedtls_sha256_ret(
      reinterpret_cast<const unsigned char*>(material),
      std::strlen(material), tag.data(), 0);
#else
  SHA256(reinterpret_cast<const unsigned char*>(material),
         std::strlen(material), tag.data());
#endif
  return tag;
}

bool isZero(const Tag& tag) {
  for (const uint8_t byte : tag) {
    if (byte != 0) return false;
  }
  return true;
}

bool equal(const Tag& a, const Tag& b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace ProfileBinding
