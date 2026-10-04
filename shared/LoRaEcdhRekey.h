#pragma once
#include <stddef.h>
#include <stdint.h>
#include <mbedtls/ctr_drbg.h>
#include "ConfigContract.h"

/*
 * X25519/ECDH rekey protocol implementation.
 *
 * The wire envelope is authenticated by the existing P2P trust anchor:
 *   byte 0     magic
 *   byte 1     version
 *   byte 2..5  epochSec, little-endian
 *   byte 6..37  ephemeral X25519 public key
 *   byte 38..69 long-term X25519 public key
 *
 * This envelope is intended to be carried inside the existing authenticated
 * LoRa beacon. It is NOT a standalone authenticated protocol.
 *
 * The implementation is compiled when
 * FIELDRADIO_LORA_ECDH_REKEY_ENABLED=1; runtime activation remains
 * controlled by `ecdhRekeyPolicy` until hardware interoperability testing is complete.
 */

namespace LoRaEcdhRekey {

constexpr size_t PUBLIC_KEY_BYTES = ConfigContract::LORA_ECDH_PUBLIC_KEY_BYTES;
constexpr size_t BEACON_BYTES = ConfigContract::LORA_ECDH_BEACON_BYTES;
constexpr size_t SHARED_SECRET_BYTES = 32;
constexpr size_t AES_KEY_BYTES = 16;
constexpr size_t HMAC_KEY_BYTES = 32;
constexpr size_t SESSION_KEY_BYTES = AES_KEY_BYTES + HMAC_KEY_BYTES;
constexpr char HKDF_DOMAIN_SEPARATOR[] = "FieldRadio-ECDH-Rekey-v1";

inline uint32_t epochNumber(uint32_t epochSec) {
  return epochSec / ConfigContract::LORA_REKEY_PERIOD_SEC;
}

inline bool sessionEpochForDelta(uint32_t epochSec, uint8_t keyEpochDelta,
                                 uint32_t& keyEpochSec) {
  keyEpochSec = 0;
  if (epochSec == 0 ||
      (keyEpochDelta != ConfigContract::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT &&
       keyEpochDelta != ConfigContract::LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS))
    return false;

  const uint32_t epoch = epochNumber(epochSec);
  if (keyEpochDelta == ConfigContract::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT) {
    keyEpochSec = epochSec;
    return true;
  }
  if (epoch == 0) return false;
  keyEpochSec = (epoch - 1U) * ConfigContract::LORA_REKEY_PERIOD_SEC;
  return true;
}

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
  out[0] = ConfigContract::LORA_ECDH_BEACON_MAGIC;
  out[1] = ConfigContract::LORA_ECDH_PROTOCOL_VERSION;
  putLe32(out + 2, beacon.epochSec);
  for (size_t i = 0; i < PUBLIC_KEY_BYTES; ++i) {
    out[6 + i] = beacon.ephemeralPublic[i];
    out[6 + PUBLIC_KEY_BYTES + i] = beacon.staticPublic[i];
  }
  return BEACON_BYTES;
}

inline bool decodeBeacon(const uint8_t* data, size_t length, Beacon& out) {
  out = Beacon{};
  if (!data || length < BEACON_BYTES ||
      data[0] != ConfigContract::LORA_ECDH_BEACON_MAGIC ||
      data[1] != ConfigContract::LORA_ECDH_PROTOCOL_VERSION ||
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

inline bool epochWithinSkew(uint32_t localEpochSec, uint32_t peerEpochSec);

inline bool authenticatedBeaconValid(const Beacon& beacon,
                                     uint32_t authenticatedSourceId,
                                     uint32_t packetEpochSec,
                                     uint32_t localEpochSec = 0) {
  if (!beacon.valid || authenticatedSourceId == 0 ||
      packetEpochSec == 0 || beacon.epochSec != packetEpochSec)
    return false;
  if (localEpochSec != 0 && !epochWithinSkew(localEpochSec, beacon.epochSec))
    return false;

  bool ephemeralNonZero = false;
  bool staticNonZero = false;
  for (size_t i = 0; i < PUBLIC_KEY_BYTES; ++i) {
    ephemeralNonZero |= beacon.ephemeralPublic[i] != 0;
    staticNonZero |= beacon.staticPublic[i] != 0;
  }
  return ephemeralNonZero && staticNonZero;
}

inline bool epochWithinSkew(uint32_t localEpochSec, uint32_t peerEpochSec) {
  if (peerEpochSec == 0) return false;
  if (localEpochSec == 0) return true;

  const uint32_t localEpoch = epochNumber(localEpochSec);
  const uint32_t peerEpoch = epochNumber(peerEpochSec);
  const uint32_t delta = localEpoch >= peerEpoch
      ? localEpoch - peerEpoch
      : peerEpoch - localEpoch;
  return delta <= 1U;
}

inline bool ecdhBeaconWireAllowed(uint8_t wireVersion, uint8_t type) {
  // G10: before V5 is implemented, only the authenticated V3 ECDH beacon
  // remains on-air when the feature is enabled.
  return wireVersion == 3U && type == ConfigContract::LORA_TYPE_NEIGHBOR_BEACON;
}

inline bool epochWithinRetention(uint32_t localEpochSec,
                                 uint32_t peerEpochSec) {
  if (localEpochSec == 0 || peerEpochSec == 0) return false;
  const uint32_t delta = localEpochSec >= peerEpochSec
      ? localEpochSec - peerEpochSec
      : peerEpochSec - localEpochSec;
  return delta <= ConfigContract::LORA_ECDH_KEY_RETENTION_SEC;
}

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED

class KeyMaterial {
public:
  static constexpr size_t KEY_BYTES = PUBLIC_KEY_BYTES;

  KeyMaterial();
  ~KeyMaterial();

  // Loads/creates the persistent long-term key and creates a fresh
  // RAM-only ephemeral key for the supplied epoch when time is known.
  bool begin(uint32_t epochSec);
  // Regenerates the RAM-only ephemeral key only when the epoch changes.
  bool ensureEphemeral(uint32_t epochSec);

  bool hasLongTermKey() const { return longTermValid_; }
  bool hasEphemeralKey() const { return ephemeralValid_; }
  uint32_t ephemeralEpoch() const { return ephemeralEpoch_; }

  const uint8_t* longTermPrivate() const { return longTermPrivate_; }
  const uint8_t* longTermPublic() const { return longTermPublic_; }
  const uint8_t* ephemeralPrivate() const { return ephemeralPrivate_; }
  const uint8_t* ephemeralPublic() const { return ephemeralPublic_; }

  // Computes the X25519 shared secret from the current RAM-only ephemeral
  // private key and the peer ephemeral public key.
  bool computeSharedSecret(const uint8_t peerEphemeralPublic[PUBLIC_KEY_BYTES],
                           uint8_t out[SHARED_SECRET_BYTES]) const;

  // HKDF-SHA256 binds the shared secret to both authenticated source IDs
  // and the epoch number, yielding AES-128 || HMAC-SHA256 keys.
  static bool deriveSessionKeyMaterial(
      const uint8_t sharedSecret[SHARED_SECRET_BYTES],
      uint32_t localSourceId, uint32_t peerSourceId, uint32_t epoch,
      uint8_t out[SESSION_KEY_BYTES]);

  // Derives and retains the peer session key for the current ephemeral epoch.
  bool deriveSessionKey(const uint8_t peerEphemeralPublic[PUBLIC_KEY_BYTES],
                        uint32_t localSourceId, uint32_t peerSourceId,
                        uint32_t epochSec);

  // Returns the current or immediately previous retained epoch key.
  bool getSessionKey(uint32_t peerSourceId, uint32_t epochSec,
                     uint8_t out[SESSION_KEY_BYTES]) const;

  void clearEphemeral();

private:
  struct SessionKeySlot {
    uint32_t epoch = 0;
    uint32_t peerSourceId = 0;
    uint8_t key[SESSION_KEY_BYTES] = {};
    bool valid = false;
  };

  void clearSessionSlot(SessionKeySlot& slot);
  bool loadOrCreateLongTerm();
  bool generateKeyPair(uint8_t privateKey[KEY_BYTES],
                       uint8_t publicKey[KEY_BYTES]);
  bool persistLongTerm(uint8_t generation,
                       const uint8_t privateKey[KEY_BYTES],
                       const uint8_t publicKey[KEY_BYTES]);
  bool validStoredKeyPair(const uint8_t privateKey[KEY_BYTES],
                          const uint8_t publicKey[KEY_BYTES]) const;
  static int ctrDrbgRng(void* context, unsigned char* output, size_t length);
  bool initCtrDrbg();

  uint8_t longTermPrivate_[KEY_BYTES] = {};
  uint8_t longTermPublic_[KEY_BYTES] = {};
  uint8_t ephemeralPrivate_[KEY_BYTES] = {};
  uint8_t ephemeralPublic_[KEY_BYTES] = {};
  uint32_t ephemeralEpoch_ = 0;
  bool longTermValid_ = false;
  bool ephemeralValid_ = false;
  mbedtls_ctr_drbg_context ctrDrbg_{};
  bool ctrDrbgReady_ = false;
  static constexpr size_t SESSION_PEER_CACHE_SIZE = 16;
  SessionKeySlot sessionSlots_[SESSION_PEER_CACHE_SIZE][2] = {};
  size_t sessionNextPeer_ = 0;
};

#endif

}  // namespace LoRaEcdhRekey
