#include "LoRaEcdhRekey.h"

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/platform_util.h>
#include <cstring>

namespace LoRaEcdhRekey {
namespace {

constexpr char NVS_NAMESPACE[] = "fr_ecdh";
constexpr char NVS_LT_PRIV[] = "lt_priv";
constexpr char NVS_LT_PUB[] = "lt_pub";
constexpr char NVS_LT_GEN[] = "lt_gen";

int espRandomRng(void*, unsigned char* output, size_t length) {
  if (!output && length != 0) return -1;

  size_t offset = 0;
  while (offset < length) {
    const uint32_t randomWord = esp_random();
    const size_t remaining = length - offset;
    const size_t copyLength = remaining < sizeof(randomWord)
        ? remaining
        : sizeof(randomWord);
    memcpy(output + offset, &randomWord, copyLength);
    offset += copyLength;
  }
  return 0;
}

bool isAllZero(const uint8_t* data, size_t length) {
  if (!data) return true;
  uint8_t accumulator = 0;
  for (size_t i = 0; i < length; ++i) accumulator |= data[i];
  return accumulator == 0;
}

}  // namespace

KeyMaterial::~KeyMaterial() {
  clearEphemeral();
  for (size_t i = 0; i < SESSION_PEER_CACHE_SIZE; ++i) {
    clearSessionSlot(sessionSlots_[i][0]);
    clearSessionSlot(sessionSlots_[i][1]);
  }
  mbedtls_platform_zeroize(longTermPrivate_, sizeof(longTermPrivate_));
  mbedtls_platform_zeroize(longTermPublic_, sizeof(longTermPublic_));
  longTermValid_ = false;
}

bool KeyMaterial::validStoredKeyPair(
    const uint8_t privateKey[KEY_BYTES],
    const uint8_t publicKey[KEY_BYTES]) const {
  return privateKey && publicKey &&
         !isAllZero(privateKey, KEY_BYTES) &&
         !isAllZero(publicKey, KEY_BYTES);
}

bool KeyMaterial::generateKeyPair(
    uint8_t privateKey[KEY_BYTES],
    uint8_t publicKey[KEY_BYTES]) {
  if (!privateKey || !publicKey) return false;

  // DECISION: use the ESP hardware RNG through the mbed TLS RNG callback.
  // Curve25519 key generation is therefore delegated to the bundled
  // mbedtls_ecdh implementation rather than implementing X25519 arithmetic
  // in application code.
  mbedtls_ecdh_context ecdh;
  mbedtls_ecdh_init(&ecdh);

  bool ok = false;
  if (mbedtls_ecp_group_load(&ecdh.grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
      mbedtls_ecdh_gen_public(&ecdh.grp, &ecdh.d, &ecdh.Q,
                              espRandomRng, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&ecdh.d, privateKey, KEY_BYTES) == 0 &&
      mbedtls_mpi_write_binary(&ecdh.Q.X, publicKey, KEY_BYTES) == 0) {
    ok = validStoredKeyPair(privateKey, publicKey);
  }

  mbedtls_ecdh_free(&ecdh);
  if (!ok) {
    mbedtls_platform_zeroize(privateKey, KEY_BYTES);
    mbedtls_platform_zeroize(publicKey, KEY_BYTES);
  }
  return ok;
}

bool KeyMaterial::persistLongTerm(
    uint8_t generation,
    const uint8_t privateKey[KEY_BYTES],
    const uint8_t publicKey[KEY_BYTES]) {
  if (!validStoredKeyPair(privateKey, publicKey)) return false;

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;

  bool ok = prefs.putBytes(NVS_LT_PRIV, privateKey, KEY_BYTES) == KEY_BYTES &&
            prefs.putBytes(NVS_LT_PUB, publicKey, KEY_BYTES) == KEY_BYTES &&
            prefs.putUChar(NVS_LT_GEN, generation) == sizeof(uint8_t);

  if (ok) {
    uint8_t readPrivate[KEY_BYTES] = {};
    uint8_t readPublic[KEY_BYTES] = {};
    const uint8_t readGeneration = prefs.getUChar(NVS_LT_GEN, 0);
    const size_t privateLength =
        prefs.getBytes(NVS_LT_PRIV, readPrivate, KEY_BYTES);
    const size_t publicLength =
        prefs.getBytes(NVS_LT_PUB, readPublic, KEY_BYTES);
    ok = privateLength == KEY_BYTES &&
         publicLength == KEY_BYTES &&
         readGeneration == generation &&
         memcmp(readPrivate, privateKey, KEY_BYTES) == 0 &&
         memcmp(readPublic, publicKey, KEY_BYTES) == 0;
    mbedtls_platform_zeroize(readPrivate, sizeof(readPrivate));
    mbedtls_platform_zeroize(readPublic, sizeof(readPublic));
  }

  prefs.end();
  return ok;
}

bool KeyMaterial::loadOrCreateLongTerm() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;

  uint8_t storedPrivate[KEY_BYTES] = {};
  uint8_t storedPublic[KEY_BYTES] = {};
  const size_t privateLength =
      prefs.getBytes(NVS_LT_PRIV, storedPrivate, KEY_BYTES);
  const size_t publicLength =
      prefs.getBytes(NVS_LT_PUB, storedPublic, KEY_BYTES);
  const uint8_t storedGeneration = prefs.getUChar(NVS_LT_GEN, 0);
  prefs.end();

  if (privateLength == KEY_BYTES &&
      publicLength == KEY_BYTES &&
      storedGeneration != 0 &&
      validStoredKeyPair(storedPrivate, storedPublic)) {
    memcpy(longTermPrivate_, storedPrivate, KEY_BYTES);
    memcpy(longTermPublic_, storedPublic, KEY_BYTES);
    mbedtls_platform_zeroize(storedPrivate, sizeof(storedPrivate));
    mbedtls_platform_zeroize(storedPublic, sizeof(storedPublic));
    longTermValid_ = true;
    return true;
  }

  mbedtls_platform_zeroize(storedPrivate, sizeof(storedPrivate));
  mbedtls_platform_zeroize(storedPublic, sizeof(storedPublic));

  uint8_t generatedPrivate[KEY_BYTES] = {};
  uint8_t generatedPublic[KEY_BYTES] = {};
  if (!generateKeyPair(generatedPrivate, generatedPublic)) return false;

  uint8_t nextGeneration = storedGeneration;
  if (nextGeneration == 0 || nextGeneration == UINT8_MAX) {
    nextGeneration = 1;
  } else {
    ++nextGeneration;
  }

  // DECISION: replace the three NVS values and verify the complete tuple
  // immediately. A later boot repairs any power-loss partial write by
  // generating another pair.
  const bool persisted =
      persistLongTerm(nextGeneration, generatedPrivate, generatedPublic);
  if (persisted) {
    memcpy(longTermPrivate_, generatedPrivate, KEY_BYTES);
    memcpy(longTermPublic_, generatedPublic, KEY_BYTES);
    longTermValid_ = true;
  }

  mbedtls_platform_zeroize(generatedPrivate, sizeof(generatedPrivate));
  mbedtls_platform_zeroize(generatedPublic, sizeof(generatedPublic));
  return persisted;
}

void KeyMaterial::clearEphemeral() {
  mbedtls_platform_zeroize(ephemeralPrivate_, sizeof(ephemeralPrivate_));
  mbedtls_platform_zeroize(ephemeralPublic_, sizeof(ephemeralPublic_));
  ephemeralEpoch_ = 0;
  ephemeralValid_ = false;
}

bool KeyMaterial::computeSharedSecret(
    const uint8_t peerEphemeralPublic[PUBLIC_KEY_BYTES],
    uint8_t out[SHARED_SECRET_BYTES]) const {
  if (!peerEphemeralPublic || !out || !ephemeralValid_) return false;

  mbedtls_ecdh_context ecdh;
  mbedtls_mpi shared;
  mbedtls_ecdh_init(&ecdh);
  mbedtls_mpi_init(&shared);

  bool ok = false;
  if (mbedtls_ecp_group_load(&ecdh.grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
      mbedtls_mpi_read_binary(&ecdh.d, ephemeralPrivate_,
                              PUBLIC_KEY_BYTES) == 0 &&
      mbedtls_mpi_lset(&ecdh.Qp.Z, 1) == 0 &&
      mbedtls_mpi_read_binary(&ecdh.Qp.X, peerEphemeralPublic,
                              PUBLIC_KEY_BYTES) == 0 &&
      mbedtls_ecdh_compute_shared(&ecdh.grp, &shared, &ecdh.Qp, &ecdh.d,
                                  espRandomRng, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&shared, out, SHARED_SECRET_BYTES) == 0) {
    ok = !isAllZero(out, SHARED_SECRET_BYTES);
  }

  mbedtls_ecdh_free(&ecdh);
  mbedtls_mpi_free(&shared);
  if (!ok) mbedtls_platform_zeroize(out, SHARED_SECRET_BYTES);
  return ok;
}

bool KeyMaterial::deriveSessionKeyMaterial(
    const uint8_t sharedSecret[SHARED_SECRET_BYTES],
    uint32_t localSourceId, uint32_t peerSourceId, uint32_t epoch,
    uint8_t out[SESSION_KEY_BYTES]) {
  if (!sharedSecret || !out || isAllZero(sharedSecret, SHARED_SECRET_BYTES))
    return false;

  constexpr size_t kDomainBytes = sizeof(HKDF_DOMAIN_SEPARATOR) - 1U;
  constexpr size_t kInfoBytes = kDomainBytes + sizeof(uint32_t) * 3U;
  constexpr uint8_t kZeroSalt[32] = {};
  uint8_t info[kInfoBytes] = {};

  memcpy(info, HKDF_DOMAIN_SEPARATOR, kDomainBytes);
  // DECISION: canonicalize the two authenticated source IDs so both peers
  // derive the same symmetric key from the same identity pair. The pair is
  // still fully bound; ordering is not a trust decision.
  const uint32_t firstSourceId =
      localSourceId < peerSourceId ? localSourceId : peerSourceId;
  const uint32_t secondSourceId =
      localSourceId < peerSourceId ? peerSourceId : localSourceId;
  putLe32(info + kDomainBytes, firstSourceId);
  putLe32(info + kDomainBytes + sizeof(uint32_t), secondSourceId);
  putLe32(info + kDomainBytes + sizeof(uint32_t) * 2U, epoch);

  const mbedtls_md_info_t* md =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const int rc = md
      ? mbedtls_hkdf(md, kZeroSalt, sizeof(kZeroSalt),
                     sharedSecret, SHARED_SECRET_BYTES,
                     info, sizeof(info), out, SESSION_KEY_BYTES)
      : -1;

  mbedtls_platform_zeroize(info, sizeof(info));
  if (rc != 0) mbedtls_platform_zeroize(out, SESSION_KEY_BYTES);
  return rc == 0;
}

void KeyMaterial::clearSessionSlot(SessionKeySlot& slot) {
  mbedtls_platform_zeroize(slot.key, sizeof(slot.key));
  slot.epoch = 0;
  slot.peerSourceId = 0;
  slot.valid = false;
}

bool KeyMaterial::deriveSessionKey(
    const uint8_t peerEphemeralPublic[PUBLIC_KEY_BYTES],
    uint32_t localSourceId, uint32_t peerSourceId, uint32_t epochSec) {
  if (!peerEphemeralPublic || !ephemeralValid_ || epochSec == 0) return false;

  const uint32_t epoch = epochNumber(epochSec);
  if (epoch != ephemeralEpoch_) {
    // DECISION: never derive with an ephemeral key from a different epoch.
    // The caller must rotate the RAM-only ephemeral key first.
    return false;
  }

  uint8_t sharedSecret[SHARED_SECRET_BYTES] = {};
  uint8_t derivedKey[SESSION_KEY_BYTES] = {};
  const bool sharedOk = computeSharedSecret(peerEphemeralPublic, sharedSecret);
  const bool hkdfOk = sharedOk &&
      deriveSessionKeyMaterial(sharedSecret, localSourceId, peerSourceId,
                               epoch, derivedKey);
  mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
  if (!hkdfOk) {
    mbedtls_platform_zeroize(derivedKey, sizeof(derivedKey));
    return false;
  }

  size_t peerSlot = SESSION_PEER_CACHE_SIZE;
  for (size_t i = 0; i < SESSION_PEER_CACHE_SIZE; ++i) {
    if (sessionSlots_[i][0].valid &&
        sessionSlots_[i][0].peerSourceId == peerSourceId) {
      peerSlot = i;
      break;
    }
    if (peerSlot == SESSION_PEER_CACHE_SIZE && !sessionSlots_[i][0].valid)
      peerSlot = i;
  }
  if (peerSlot == SESSION_PEER_CACHE_SIZE) {
    peerSlot = sessionNextPeer_;
    sessionNextPeer_ = (sessionNextPeer_ + 1U) % SESSION_PEER_CACHE_SIZE;
    clearSessionSlot(sessionSlots_[peerSlot][0]);
    clearSessionSlot(sessionSlots_[peerSlot][1]);
  }

  SessionKeySlot& current = sessionSlots_[peerSlot][0];
  SessionKeySlot& previous = sessionSlots_[peerSlot][1];
  if (current.valid && current.epoch != epoch) {
    clearSessionSlot(previous);
    previous = current;
    mbedtls_platform_zeroize(current.key, sizeof(current.key));
    current.epoch = 0;
    current.peerSourceId = 0;
    current.valid = false;
  }

  memcpy(current.key, derivedKey, sizeof(derivedKey));
  current.epoch = epoch;
  current.peerSourceId = peerSourceId;
  current.valid = true;
  mbedtls_platform_zeroize(derivedKey, sizeof(derivedKey));
  return true;
}

bool KeyMaterial::getSessionKey(uint32_t peerSourceId, uint32_t epochSec,
                                uint8_t out[SESSION_KEY_BYTES]) const {
  if (!out || epochSec == 0 || peerSourceId == 0) return false;
  const uint32_t epoch = epochNumber(epochSec);
  for (size_t i = 0; i < SESSION_PEER_CACHE_SIZE; ++i) {
    for (size_t generation = 0; generation < 2; ++generation) {
      const SessionKeySlot& slot = sessionSlots_[i][generation];
      if (slot.valid && slot.peerSourceId == peerSourceId &&
          slot.epoch == epoch) {
        memcpy(out, slot.key, SESSION_KEY_BYTES);
        return true;
      }
    }
  }
  return false;
}

bool KeyMaterial::begin(uint32_t epochSec) {
  clearEphemeral();
  for (size_t i = 0; i < SESSION_PEER_CACHE_SIZE; ++i) {
    clearSessionSlot(sessionSlots_[i][0]);
    clearSessionSlot(sessionSlots_[i][1]);
  }
  sessionNextPeer_ = 0;
  longTermValid_ = false;
  if (!loadOrCreateLongTerm()) return false;

  if (epochSec == 0) {
    // Time is not usable yet. Do not mint an epoch-0 key that could be
    // accidentally advertised as a real epoch.
    return true;
  }
  return ensureEphemeral(epochSec);
}

bool KeyMaterial::ensureEphemeral(uint32_t epochSec) {
  if (!longTermValid_ || epochSec == 0) return false;

  const uint32_t epoch = epochNumber(epochSec);
  if (ephemeralValid_ && ephemeralEpoch_ == epoch) return true;

  uint8_t newPrivate[KEY_BYTES] = {};
  uint8_t newPublic[KEY_BYTES] = {};
  if (!generateKeyPair(newPrivate, newPublic)) return false;

  // DECISION: generate into temporary storage first. If generation fails,
  // the currently valid ephemeral key remains usable until its replacement
  // succeeds; the old key is erased only after a complete new pair exists.
  mbedtls_platform_zeroize(ephemeralPrivate_, sizeof(ephemeralPrivate_));
  mbedtls_platform_zeroize(ephemeralPublic_, sizeof(ephemeralPublic_));
  memcpy(ephemeralPrivate_, newPrivate, KEY_BYTES);
  memcpy(ephemeralPublic_, newPublic, KEY_BYTES);
  ephemeralEpoch_ = epoch;
  ephemeralValid_ = true;

  mbedtls_platform_zeroize(newPrivate, sizeof(newPrivate));
  mbedtls_platform_zeroize(newPublic, sizeof(newPublic));
  return true;
}

}  // namespace LoRaEcdhRekey

#endif  // FIELDRADIO_LORA_ECDH_REKEY_ENABLED
