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

}  // namespace

int main() {
  test_beacon_roundtrip();
  test_rejects_invalid_envelope();
  test_epoch_retention_window();
  return 0;
}
