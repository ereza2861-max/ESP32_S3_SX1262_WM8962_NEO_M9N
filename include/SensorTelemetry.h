#pragma once
#include <cstddef>
#include <cstdint>

namespace SensorTelemetry {

constexpr uint8_t MAGIC = 0x53; // "S"
constexpr uint8_t VERSION = 1;

struct Decoded {
  uint32_t nodeId = 0;
  uint16_t sensorId = 0;
  float value = 0.0f;
  uint8_t quality = 0;
  uint64_t timestampMs = 0;
  uint32_t sampleId = 0;
  uint8_t schemaVersion = 0;
  uint32_t firmwareVersion = 0;
  uint32_t sourceSequence = 0;
  uint32_t originNodeId = 0;
};
constexpr size_t PAYLOAD_BYTES = 40;
constexpr size_t SERIALIZED_FIELDS_BYTES = 22;
constexpr size_t PADDING_SAMPLE_ID_OFFSET = 23;
constexpr size_t PADDING_SCHEMA_VERSION_OFFSET = 27;
constexpr size_t PADDING_FIRMWARE_VERSION_OFFSET = 28;
constexpr size_t PADDING_SOURCE_SEQUENCE_OFFSET = 32;
constexpr size_t PADDING_ORIGIN_NODE_ID_OFFSET = 36;

// Wire format is little-endian: magic(1), version(1), nodeId(4), sensorId(2),
// value(float32, 4), quality(1), timestampMs(uint64, 8), crc16(2), padding(18).
// Padding is additive: sampleId(4), schemaVersion(1), firmwareVersion(4),
// sourceSequence(4), and originNodeId(4) are present when non-zero; legacy
// zero padding decodes as 0.
// CRC is CRC-16/CCITT-FALSE over bytes 0..19.
size_t serializeSensorTelemetry(uint8_t out[PAYLOAD_BYTES], uint32_t nodeId,
                                uint16_t sensorId, float value, uint8_t quality,
                                uint64_t timestampMs);
size_t serializeSensorTelemetry(uint8_t out[PAYLOAD_BYTES], uint32_t nodeId,
                                uint16_t sensorId, float value, uint8_t quality,
                                uint64_t timestampMs, uint32_t sampleId,
                                uint8_t schemaVersion = 0, uint32_t firmwareVersion = 0,
                                uint32_t sourceSequence = 0, uint32_t originNodeId = 0);
bool deserializeSensorTelemetry(const uint8_t* in, size_t len, Decoded& out);

bool shouldReportSensor(float oldV, float newV, uint32_t lastMs, uint32_t nowMs,
                        float threshold, uint32_t periodMs);

} // namespace SensorTelemetry
