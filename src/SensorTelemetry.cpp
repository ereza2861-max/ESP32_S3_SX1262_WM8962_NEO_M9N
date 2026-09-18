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
  return PAYLOAD_BYTES;
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
