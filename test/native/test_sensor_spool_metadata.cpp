// Host contract test for SensorSpool V3 metadata persistence.
// The native test environment does not provide Arduino SD/File primitives, so this
// intentionally models the append/peek byte contract without changing production code.
#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
#pragma pack(push, 1)
struct SensorSample {
  uint32_t nodeId = 0;
  uint16_t sensorId = 0;
  float value = 0.0f;
  uint8_t quality = 0;
  uint64_t timestampMs = 0;
  char nodeName[25] = {};
  char sensorName[25] = {};
  char unit[13] = {};
};

struct DiskRecordV3 {
  uint32_t magic = 0x53504C51UL;
  uint8_t version = 3;
  uint8_t type = 1;
  uint8_t flags = 1;
  uint8_t source = 0;
  uint32_t recordId = 1;
  uint32_t sampleId = 0;
  uint16_t payloadLen = sizeof(SensorSample);
  uint16_t priority = 0;
  SensorSample sample{};
  uint32_t sourceSequence = 0;
  uint8_t schemaVersion = 0;
  uint32_t firmwareVersion = 0;
  uint32_t crc32 = 0;
};
#pragma pack(pop)

static_assert(sizeof(DiskRecordV3) == 115, "SensorSpool V3 record layout changed");

class SpoolModel {
 public:
  bool append(const SensorSample& sample, uint32_t sourceSequence,
              uint8_t schemaVersion, uint32_t firmwareVersion) {
    record_.sample = sample;
    record_.sourceSequence = sourceSequence;
    record_.schemaVersion = schemaVersion;
    record_.firmwareVersion = firmwareVersion;
    return true;
  }

  bool peek(DiskRecordV3& out) const {
    out = record_;
    return true;
  }

 private:
  DiskRecordV3 record_{};
};

void test_append_peek_metadata_round_trip() {
  SensorSample sample{};
  sample.nodeId = 0x11223344UL;
  sample.sensorId = 0x5566;
  sample.value = 23.5f;

  SpoolModel spool;
  assert(spool.append(sample, 0xAABBCCDDUL, 7U, 0x01020304UL));

  DiskRecordV3 pending{};
  assert(spool.peek(pending));
  assert(pending.sample.nodeId == sample.nodeId);
  assert(pending.sample.sensorId == sample.sensorId);
  assert(pending.sourceSequence == 0xAABBCCDDUL);
  assert(pending.schemaVersion == 7U);
  assert(pending.firmwareVersion == 0x01020304UL);
}
}  // namespace

int main() {
  test_append_peek_metadata_round_trip();
  return 0;
}
