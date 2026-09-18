#pragma once
#include <cstddef>
#include <cstdint>

namespace SensorTelemetry {

constexpr uint8_t MAGIC = 0x53; // "S"
constexpr uint8_t VERSION = 1;
constexpr size_t PAYLOAD_BYTES = 40;
constexpr size_t SERIALIZED_FIELDS_BYTES = 22;

// Wire format is little-endian: magic(1), version(1), nodeId(4), sensorId(2),
// value(float32, 4), quality(1), timestampMs(uint64, 8), crc16(2), padding(18).
// CRC is CRC-16/CCITT-FALSE over bytes 0..19; padding is always zero.
size_t serializeSensorTelemetry(uint8_t out[PAYLOAD_BYTES], uint32_t nodeId,
                                uint16_t sensorId, float value, uint8_t quality,
                                uint64_t timestampMs);

bool shouldReportSensor(float oldV, float newV, uint32_t lastMs, uint32_t nowMs,
                        float threshold, uint32_t periodMs);

} // namespace SensorTelemetry
