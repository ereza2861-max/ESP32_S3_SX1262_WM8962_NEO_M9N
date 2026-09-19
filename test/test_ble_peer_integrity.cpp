#include <cassert>
#include <cstring>
#include "BlePeerStore.h"

int main() {
  static_assert(BlePeerStore::V1_BYTES == 64, "legacy layout changed");
  static_assert(BlePeerStore::V2_BYTES == 90, "v2 layout must include the explicit 16-byte IRK");

  const char* loraKey = "00112233445566778899aabbccddeeff";
  BlePeerStore::PeerRecordV2 record{};
  record.magic = BlePeerStore::MAGIC;
  record.version = BlePeerStore::VERSION;
  BlePeerStore::setPasskey(record, 123456);
  record.identity = {{0x06, 0x05, 0x04, 0x03, 0x02, 0x01}, 0};
  record.lastRpa = {{0x94, 0x81, 0x70, 0xba, 0xfa, 0x0d}, 1};
  std::strncpy(record.name, "sensor-01", sizeof(record.name) - 1);
  record.updatedEpoch = 1700000000UL;
  for (size_t i = 0; i < sizeof(record.irk); ++i)
    record.irk[i] = static_cast<uint8_t>(i);

  const auto expectedIdentity = record.identity;
  const auto expectedRpa = record.lastRpa;
  const auto expectedIrk = record.irk;
  assert(BlePeerStore::sealV2(record, loraKey));

  BlePeerStore::PeerRecordV2 roundTrip = record;
  assert(BlePeerStore::openV2(roundTrip, loraKey));
  assert(BlePeerStore::passkey(roundTrip) == 123456U);
  assert(roundTrip.identity == expectedIdentity);
  assert(roundTrip.lastRpa == expectedRpa);
  assert(std::memcmp(roundTrip.irk, expectedIrk, sizeof(record.irk)) == 0);

  BlePeerStore::PeerRecordV2 tampered = record;
  tampered.mac[0] ^= 0x01;
  tampered.crc32 = BlePeerStore::crc32(
      reinterpret_cast<const uint8_t*>(&tampered),
      offsetof(BlePeerStore::PeerRecordV2, crc32));
  assert(!BlePeerStore::openV2(tampered, loraKey));

  BlePeerStore::PeerRecordV2 migrated{};
  BlePeerStore::PeerRecordV1 legacy{};
  legacy.magic = BlePeerStore::MAGIC;
  legacy.passkey = 654321;
  legacy.identity = record.identity;
  legacy.lastRpa = record.lastRpa;
  std::strncpy(legacy.name, "legacy", sizeof(legacy.name) - 1);
  legacy.updatedEpoch = 1700000000UL;
  uint32_t legacyCrc = 2166136261UL;
  const uint8_t* legacyBytes = reinterpret_cast<const uint8_t*>(&legacy);
  for (size_t i = 0; i < offsetof(BlePeerStore::PeerRecordV1, crc); ++i) {
    legacyCrc ^= legacyBytes[i];
    legacyCrc *= 16777619UL;
  }
  legacy.crc = legacyCrc;
  assert(BlePeerStore::migrateV1(legacy, migrated, loraKey));

  BlePeerStore::PeerRecordV2 migratedPlain = migrated;
  assert(BlePeerStore::openV2(migratedPlain, loraKey));
  assert(BlePeerStore::passkey(migratedPlain) == 654321U);
  return 0;
}
