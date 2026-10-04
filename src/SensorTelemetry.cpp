#include "SensorTelemetry.h"
#include <cmath>
#include <cstring>

namespace SensorTelemetry {
namespace {
void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}
void putU32(uint8_t* out, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8U * i));
}
void putU64(uint8_t* out, uint64_t value) {
  for (uint8_t i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8U * i));
}
uint16_t crc16Ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1) ^ 0x1021U)
                             : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}
} // namespace

size_t serializeSensorTelemetry(uint8_t out[PAYLOAD_BYTES], uint32_t nodeId,
                                uint16_t sensorId, float value, uint8_t quality,
                                uint64_t timestampMs) {
  return serializeSensorTelemetry(out, nodeId, sensorId, value, quality, timestampMs, 0);
}

size_t serializeSensorTelemetry(uint8_t out[PAYLOAD_BYTES], uint32_t nodeId,
                                uint16_t sensorId, float value, uint8_t quality,
                                uint64_t timestampMs, uint32_t sampleId,
                                uint8_t schemaVersion, uint32_t firmwareVersion,
                                uint32_t sourceSequence, uint32_t originNodeId) {
  if (!out || nodeId == 0 || sensorId == 0 || !std::isfinite(value)) return 0;
  std::memset(out, 0, PAYLOAD_BYTES);
  out[0] = MAGIC;
  out[1] = VERSION;
  putU32(out + 2, nodeId);
  putU16(out + 6, sensorId);
  std::memcpy(out + 8, &value, sizeof(value));
  out[12] = quality;
  putU64(out + 13, timestampMs);
  putU16(out + 21, crc16Ccitt(out, SERIALIZED_FIELDS_BYTES - 2));
  putU32(out + PADDING_SAMPLE_ID_OFFSET, sampleId);
  out[PADDING_SCHEMA_VERSION_OFFSET] = schemaVersion;
  putU32(out + PADDING_FIRMWARE_VERSION_OFFSET, firmwareVersion);
  putU32(out + PADDING_SOURCE_SEQUENCE_OFFSET, sourceSequence);
  putU32(out + PADDING_ORIGIN_NODE_ID_OFFSET, originNodeId);
  return PAYLOAD_BYTES;
}

bool deserializeSensorTelemetry(const uint8_t* in, size_t len, Decoded& out) {
  if (!in || len != PAYLOAD_BYTES || in[0] != MAGIC || in[1] != VERSION) return false;
  if (crc16Ccitt(in, SERIALIZED_FIELDS_BYTES - 2) !=
      static_cast<uint16_t>(in[21] | (static_cast<uint16_t>(in[22]) << 8))) return false;
  std::memcpy(&out.nodeId, in + 2, sizeof(out.nodeId));
  std::memcpy(&out.sensorId, in + 6, sizeof(out.sensorId));
  std::memcpy(&out.value, in + 8, sizeof(out.value));
  out.quality = in[12];
  std::memcpy(&out.timestampMs, in + 13, sizeof(out.timestampMs));
  std::memcpy(&out.sampleId, in + PADDING_SAMPLE_ID_OFFSET, sizeof(out.sampleId));
  out.schemaVersion = in[PADDING_SCHEMA_VERSION_OFFSET];
  std::memcpy(&out.firmwareVersion, in + PADDING_FIRMWARE_VERSION_OFFSET,
              sizeof(out.firmwareVersion));
  std::memcpy(&out.sourceSequence, in + PADDING_SOURCE_SEQUENCE_OFFSET,
              sizeof(out.sourceSequence));
  std::memcpy(&out.originNodeId, in + PADDING_ORIGIN_NODE_ID_OFFSET,
              sizeof(out.originNodeId));
  return out.nodeId != 0 && out.sensorId != 0 && std::isfinite(out.value);
}

bool shouldReportSensor(float oldV, float newV, uint32_t lastMs, uint32_t nowMs,
                        float threshold, uint32_t periodMs) {
  if (!std::isfinite(newV) || !std::isfinite(threshold) || threshold < 0.0f) return false;
  if (lastMs == 0 || static_cast<uint32_t>(nowMs - lastMs) >= periodMs) return true;
  if (!std::isfinite(oldV)) return true;
  const float denominator = fmaxf(fabsf(oldV), 1.0f);
  return fabsf(newV - oldV) / denominator >= threshold;
}

} // namespace SensorTelemetry
