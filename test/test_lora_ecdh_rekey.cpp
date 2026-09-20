#include <cassert>
#include <cstring>
#include "LoRaEcdhRekey.h"

namespace {

void test_beacon_roundtrip() {
  LoRaEcdhRekey::Beacon in{};
  in.epochSec = 1700000000UL;
  in.valid = true;
  for (size_t i = 0; i < LoRaEcdhRekey::PUBLIC_KEY_BYTES; ++i) {
    in.ephemeralPublic[i] = static_cast<uint8_t>(i);
    in.staticPublic[i] = static_cast<uint8_t>(0xA0U + i);
  }

  uint8_t wire[LoRaEcdhRekey::BEACON_BYTES] = {};
  assert(LoRaEcdhRekey::encodeBeacon(in, wire, sizeof(wire)) ==
         LoRaEcdhRekey::BEACON_BYTES);

  LoRaEcdhRekey::Beacon out{};
  assert(LoRaEcdhRekey::decodeBeacon(wire, sizeof(wire), out));
  assert(out.valid);
  assert(out.epochSec == in.epochSec);
  assert(std::memcmp(out.ephemeralPublic, in.ephemeralPublic,
                     LoRaEcdhRekey::PUBLIC_KEY_BYTES) == 0);
  assert(std::memcmp(out.staticPublic, in.staticPublic,
                     LoRaEcdhRekey::PUBLIC_KEY_BYTES) == 0);
}

void test_rejects_invalid_envelope() {
  uint8_t wire[LoRaEcdhRekey::BEACON_BYTES] = {};
  LoRaEcdhRekey::Beacon out{};
  assert(!LoRaEcdhRekey::decodeBeacon(wire, sizeof(wire), out));
  wire[0] = Config::LORA_ECDH_BEACON_MAGIC;
  wire[1] = Config::LORA_ECDH_PROTOCOL_VERSION;
  assert(!LoRaEcdhRekey::decodeBeacon(wire, sizeof(wire), out));
}

void test_epoch_retention_window() {
  constexpr uint32_t now = 10UL * Config::LORA_REKEY_PERIOD_SEC;
  assert(LoRaEcdhRekey::epochWithinRetention(
      now, now - 2UL * Config::LORA_REKEY_PERIOD_SEC));
  assert(!LoRaEcdhRekey::epochWithinRetention(
      now, now - 2UL * Config::LORA_REKEY_PERIOD_SEC - 1UL));
}

void test_authenticated_beacon_binding() {
  constexpr uint32_t epoch = 100U * Config::LORA_REKEY_PERIOD_SEC;
  LoRaEcdhRekey::Beacon beacon{};
  beacon.valid = true;
  beacon.epochSec = epoch;
  beacon.ephemeralPublic[0] = 1;
  beacon.staticPublic[0] = 2;

  assert(LoRaEcdhRekey::authenticatedBeaconValid(
      beacon, 0x1234U, epoch, epoch));
  assert(!LoRaEcdhRekey::authenticatedBeaconValid(
      beacon, 0, epoch, epoch));
  assert(!LoRaEcdhRekey::authenticatedBeaconValid(
      beacon, 0x1234U, epoch + Config::LORA_REKEY_PERIOD_SEC, epoch));

  beacon.staticPublic[0] = 0;
  beacon.ephemeralPublic[0] = 0;
  assert(!LoRaEcdhRekey::authenticatedBeaconValid(
      beacon, 0x1234U, epoch));
}

void test_epoch_skew() {
  constexpr uint32_t period = Config::LORA_REKEY_PERIOD_SEC;
  const uint32_t epoch = 100U * period;

  assert(LoRaEcdhRekey::epochWithinSkew(epoch, epoch));
  assert(LoRaEcdhRekey::epochWithinSkew(epoch, epoch + period));
  assert(LoRaEcdhRekey::epochWithinSkew(epoch, epoch - period));
  assert(!LoRaEcdhRekey::epochWithinSkew(
      epoch, epoch + 2U * period));
  assert(!LoRaEcdhRekey::epochWithinSkew(
      epoch, epoch - 2U * period));
  assert(LoRaEcdhRekey::epochWithinSkew(0, epoch));
}

void test_ecdh_downgrade_wire_policy() {
  assert(LoRaEcdhRekey::ecdhBeaconWireAllowed(
      3U, Config::LORA_TYPE_NEIGHBOR_BEACON));
  assert(!LoRaEcdhRekey::ecdhBeaconWireAllowed(
      2U, Config::LORA_TYPE_NEIGHBOR_BEACON));
  assert(!LoRaEcdhRekey::ecdhBeaconWireAllowed(
      4U, Config::LORA_TYPE_NEIGHBOR_BEACON));
  assert(!LoRaEcdhRekey::ecdhBeaconWireAllowed(
      3U, Config::LORA_TYPE_TEXT));
}

void test_v5_wire_contract() {
  assert(Config::LORA_PROTOCOL_VERSION_ECDH == 5U);
  assert(Config::LORA_ECDH_V5_HEADER_BYTES == 20U);
  assert(Config::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT == 0U);
  assert(Config::LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS == 1U);
  assert(!LoRaEcdhRekey::ecdhBeaconWireAllowed(
      Config::LORA_PROTOCOL_VERSION_ECDH, Config::LORA_TYPE_NEIGHBOR_BEACON));

  constexpr uint32_t epoch =
      10U * Config::LORA_REKEY_PERIOD_SEC + 123U;
  uint32_t current = 0;
  uint32_t previous = 0;
  assert(LoRaEcdhRekey::sessionEpochForDelta(
      epoch, Config::LORA_ECDH_KEY_EPOCH_DELTA_CURRENT, current));
  assert(LoRaEcdhRekey::sessionEpochForDelta(
      epoch, Config::LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS, previous));
  assert(current == epoch);
  assert(previous == 9U * Config::LORA_REKEY_PERIOD_SEC);

  uint32_t invalid = 0;
  assert(!LoRaEcdhRekey::sessionEpochForDelta(
      Config::LORA_REKEY_PERIOD_SEC,
      Config::LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS, invalid));
}

void test_epoch_number_changes_only_at_boundary() {
  constexpr uint32_t period = Config::LORA_REKEY_PERIOD_SEC;
  assert(LoRaEcdhRekey::epochNumber(period - 1U) == 0U);
  assert(LoRaEcdhRekey::epochNumber(period) == 1U);
  assert(LoRaEcdhRekey::epochNumber(2U * period) == 2U);
  assert(LoRaEcdhRekey::epochNumber(2U * period + period - 1U) == 2U);
}

#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
void test_x25519_shared_secret_matches() {
  LoRaEcdhRekey::KeyMaterial a;
  LoRaEcdhRekey::KeyMaterial b;
  constexpr uint32_t epoch = 10U * Config::LORA_REKEY_PERIOD_SEC;
  assert(a.begin(epoch));
  assert(b.begin(epoch));

  uint8_t sharedA[LoRaEcdhRekey::SHARED_SECRET_BYTES] = {};
  uint8_t sharedB[LoRaEcdhRekey::SHARED_SECRET_BYTES] = {};
  assert(a.computeSharedSecret(b.ephemeralPublic(), sharedA));
  assert(b.computeSharedSecret(a.ephemeralPublic(), sharedB));
  assert(std::memcmp(sharedA, sharedB, sizeof(sharedA)) == 0);
}

void test_hkdf_binds_pair_and_epoch() {
  uint8_t shared[LoRaEcdhRekey::SHARED_SECRET_BYTES] = {};
  for (size_t i = 0; i < sizeof(shared); ++i)
    shared[i] = static_cast<uint8_t>(0x10U + i);

  uint8_t keyA[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  uint8_t keyB[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  uint8_t keyOtherEpoch[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};

  assert(LoRaEcdhRekey::KeyMaterial::deriveSessionKeyMaterial(
      shared, 0x1001U, 0x2002U, 77U, keyA));
  assert(LoRaEcdhRekey::KeyMaterial::deriveSessionKeyMaterial(
      shared, 0x2002U, 0x1001U, 77U, keyB));
  assert(std::memcmp(keyA, keyB, sizeof(keyA)) == 0);

  assert(LoRaEcdhRekey::KeyMaterial::deriveSessionKeyMaterial(
      shared, 0x1001U, 0x2002U, 78U, keyOtherEpoch));
  assert(std::memcmp(keyA, keyOtherEpoch, sizeof(keyA)) != 0);
}

void test_multi_peer_key_retention() {
  LoRaEcdhRekey::KeyMaterial gateway;
  LoRaEcdhRekey::KeyMaterial peerA;
  LoRaEcdhRekey::KeyMaterial peerB;
  constexpr uint32_t epoch = 12U * Config::LORA_REKEY_PERIOD_SEC;
  assert(gateway.begin(epoch));
  assert(peerA.begin(epoch));
  assert(peerB.begin(epoch));

  assert(gateway.deriveSessionKey(peerA.ephemeralPublic(), 0x1000U, 0x2000U, epoch));
  assert(gateway.deriveSessionKey(peerB.ephemeralPublic(), 0x1000U, 0x3000U, epoch));

  uint8_t keyA[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  uint8_t keyB[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  assert(gateway.getSessionKey(0x2000U, epoch, keyA));
  assert(gateway.getSessionKey(0x3000U, epoch, keyB));
  assert(std::memcmp(keyA, keyB, sizeof(keyA)) != 0);
}

void test_epoch_key_retention() {
  LoRaEcdhRekey::KeyMaterial a;
  LoRaEcdhRekey::KeyMaterial b;
  constexpr uint32_t epoch1 = 10U * Config::LORA_REKEY_PERIOD_SEC;
  constexpr uint32_t epoch2 = 11U * Config::LORA_REKEY_PERIOD_SEC;

  assert(a.begin(epoch1));
  assert(b.begin(epoch1));
  assert(a.deriveSessionKey(b.ephemeralPublic(), 0x1001U, 0x2002U, epoch1));
  assert(b.deriveSessionKey(a.ephemeralPublic(), 0x2002U, 0x1001U, epoch1));

  uint8_t keyA[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  uint8_t keyB[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  assert(a.getSessionKey(0x2002U, epoch1, keyA));
  assert(b.getSessionKey(0x1001U, epoch1, keyB));
  assert(std::memcmp(keyA, keyB, sizeof(keyA)) == 0);

  assert(a.ensureEphemeral(epoch2));
  assert(b.ensureEphemeral(epoch2));
  assert(a.deriveSessionKey(b.ephemeralPublic(), 0x1001U, 0x2002U, epoch2));
  assert(b.deriveSessionKey(a.ephemeralPublic(), 0x2002U, 0x1001U, epoch2));

  uint8_t previousA[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  uint8_t currentA[LoRaEcdhRekey::SESSION_KEY_BYTES] = {};
  assert(a.getSessionKey(0x2002U, epoch1, previousA));
  assert(a.getSessionKey(0x2002U, epoch2, currentA));
  assert(std::memcmp(previousA, keyA, sizeof(previousA)) == 0);
  assert(std::memcmp(currentA, keyA, sizeof(currentA)) != 0);
}
#endif

}  // namespace

int main() {
  test_beacon_roundtrip();
  test_rejects_invalid_envelope();
  test_epoch_retention_window();
  test_authenticated_beacon_binding();
  test_epoch_skew();
  test_ecdh_downgrade_wire_policy();
  test_epoch_number_changes_only_at_boundary();
  test_v5_wire_contract();
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  test_x25519_shared_secret_matches();
  test_hkdf_binds_pair_and_epoch();
  test_epoch_key_retention();
  test_multi_peer_key_retention();
#endif
  return 0;
}
