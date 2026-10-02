// Host contract test for the additive MQTT sensor JSON metadata fields.
#include <cassert>
#include <cstdint>
#include <string>

namespace {
struct SensorSample {
  uint32_t nodeId = 0;
  uint16_t sensorId = 0;
  uint32_t sourceSequence = 0;
  uint8_t schemaVersion = 0;
  uint32_t firmwareVersion = 0;
};

std::string makePayload(const SensorSample& sample) {
  return std::string("{\"node\":") + std::to_string(sample.nodeId) +
         ",\"sensor\":" + std::to_string(sample.sensorId) +
         ",\"sample_id\":0" +
         ",\"source_sequence\":" + std::to_string(sample.sourceSequence) +
         ",\"schema_version\":" + std::to_string(sample.schemaVersion) +
         ",\"firmware_version\":" + std::to_string(sample.firmwareVersion) +
         "}";
}

void test_metadata_fields_are_serialized() {
  SensorSample sample{0x11223344UL, 0x5566, 0xAABBCCDDUL, 7U, 0x01020304UL};
  const std::string payload = makePayload(sample);
  assert(payload.find("\"source_sequence\":2864434397") != std::string::npos);
  assert(payload.find("\"schema_version\":7") != std::string::npos);
  assert(payload.find("\"firmware_version\":16909060") != std::string::npos);
}
}  // namespace

int main() {
  test_metadata_fields_are_serialized();
  return 0;
}
