#pragma once

#include "SensorProtocol.h"
#include <cstddef>
#include <cstdint>

namespace BlePeerStore {

constexpr uint32_t MAGIC = 0x42504531UL; // "BPE1", retained across format versions
constexpr uint8_t VERSION = 2;
constexpr size_t V1_BYTES = 64;
constexpr size_t V2_BYTES = 90;
constexpr size_t MAC_BYTES = 16;

#pragma pack(push, 1)
struct PeerRecordV1 {
  uint32_t magic = 0;
  uint32_t passkey = 0;
  SensorProtocol::BleAddress identity{};
  SensorProtocol::BleAddress lastRpa{};
  char name[SensorProtocol::MAX_NODE_NAME_BYTES]{};
  uint32_t updatedEpoch = 0;
  uint32_t crc = 0;
  uint8_t reserved[10]{};
};

struct PeerRecordV2 {
  uint32_t magic = MAGIC;
  uint8_t version = VERSION;
  uint8_t reserved[3]{};
  uint8_t passkey[4]{};
  SensorProtocol::BleAddress identity{};
  SensorProtocol::BleAddress lastRpa{};
  char name[SensorProtocol::MAX_NODE_NAME_BYTES]{};
  uint32_t updatedEpoch = 0;
  uint8_t irk[16]{};
  uint8_t mac[MAC_BYTES]{};
  uint32_t crc32 = 0;
};
#pragma pack(pop)

static_assert(sizeof(PeerRecordV1) == V1_BYTES, "legacy peer record layout changed");
static_assert(sizeof(PeerRecordV2) == V2_BYTES, "peer v2 record layout changed");

bool deriveMasterKey(const char* loraKeyHex, uint8_t out[16]);
uint32_t crc32(const uint8_t* data, size_t len);
bool sealV2(PeerRecordV2& record, const char* loraKeyHex);
bool openV2(PeerRecordV2& record, const char* loraKeyHex);
bool migrateV1(const PeerRecordV1& legacy, PeerRecordV2& out, const char* loraKeyHex);
bool validV1(const PeerRecordV1& legacy);
uint32_t passkey(const PeerRecordV2& record);
void setPasskey(PeerRecordV2& record, uint32_t passkey);
bool matchesRpa(const SensorProtocol::BleAddress& rpa, const uint8_t irk[16]);

}  // namespace BlePeerStore
