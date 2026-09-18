#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Config.h"

/*
 * FASE 4 X25519/ECDH scaffold.
 *
 * The wire envelope is deliberately specified here before enabling crypto:
 *   byte 0     magic
 *   byte 1     version
 *   byte 2..5  epochSec, little-endian
 *   byte 6..37  ephemeral X25519 public key
 *   byte 38..69 long-term X25519 public key
 *
 * This envelope is intended to be carried inside the existing authenticated
 * LoRa beacon. It is NOT a standalone authenticated protocol.
 *
 * Crypto implementation is intentionally inactive until
 * FIELDRADIO_LORA_ECDH_REKEY_ENABLED=1 is selected and hardware
 * interoperability testing is complete.
 */

namespace LoRaEcdhRekey {

constexpr size_t PUBLIC_KEY_BYTES = Config::LORA_ECDH_PUBLIC_KEY_BYTES;
constexpr size_t BEACON_BYTES = Config::LORA_ECDH_BEACON_BYTES;

struct Beacon {
  uint32_t epochSec = 0;
  uint8_t ephemeralPublic[PUBLIC_KEY_BYTES] = {};
  uint8_t staticPublic[PUBLIC_KEY_BYTES] = {};
  bool valid = false;
};

inline void putLe32(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value >> 16);
  dst[3] = static_cast<uint8_t>(value >> 24);
}

inline uint32_t getLe32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

inline size_t encodeBeacon(const Beacon& beacon, uint8_t* out, size_t capacity) {
  if (!out || capacity < BEACON_BYTES || !beacon.valid) return 0;
  out[0] = Config::LORA_ECDH_BEACON_MAGIC;
  out[1] = Config::LORA_ECDH_PROTOCOL_VERSION;
  putLe32(out + 2, beacon.epochSec);
  for (size_t i = 0; i < PUBLIC_KEY_BYTES; ++i) {
    out[6 + i] = beacon.ephemeralPublic[i];
    out[6 + PUBLIC_KEY_BYTES + i] = beacon.staticPublic[i];
  }
  return BEACON_BYTES;
}

inline bool decodeBeacon(const uint8_t* data, size_t length, Beacon& out) {
  out = Beacon{};
  if (!data || length != BEACON_BYTES ||
      data[0] != Config::LORA_ECDH_BEACON_MAGIC ||
      data[1] != Config::LORA_ECDH_PROTOCOL_VERSION ||
      getLe32(data + 2) == 0) {
    return false;
  }
  out.epochSec = getLe32(data + 2);
  for (size_t i = 0; i < PUBLIC_KEY_BYTES; ++i) {
    out.ephemeralPublic[i] = data[6 + i];
    out.staticPublic[i] = data[6 + PUBLIC_KEY_BYTES + i];
  }
  out.valid = true;
  return true;
}

inline bool epochWithinRetention(uint32_t localEpochSec,
                                 uint32_t peerEpochSec) {
  if (localEpochSec == 0 || peerEpochSec == 0) return false;
  const uint32_t delta = localEpochSec >= peerEpochSec
      ? localEpochSec - peerEpochSec
      : peerEpochSec - localEpochSec;
  return delta <= Config::LORA_ECDH_KEY_RETENTION_SEC;
}

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
/*
 * TODO(EC-DH-BRINGUP):
 * - Use mbedtls_ecdh with MBEDTLS_ECP_DP_CURVE25519.
 * - Generate/load a 32-byte long-term private key from NVS.
 * - Generate a fresh ephemeral keypair at each epoch and erase the previous
 *   ephemeral private key after its retention window.
 * - Compute X25519 shared secret.
 * - HKDF-SHA256 with an explicit domain separator, peer identity and epoch.
 * - Keep current + previous epoch session keys for 2 epochs.
 * - Authenticate peer static public key through the existing authenticated
 *   beacon/master-key trust relationship.
 * - Reject stale/future epochs outside the documented clock-skew window.
 *
 * The actual calls are deliberately not provided in this scaffold because
 * ESP-IDF/Mbed TLS major-version API differences must be validated against
 * the exact PlatformIO framework before activation.
 */
#endif

}  // namespace LoRaEcdhRekey
