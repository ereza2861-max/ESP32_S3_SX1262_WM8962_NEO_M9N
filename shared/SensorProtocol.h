#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace SensorProtocol {

// Protocol v1 is BLE/GATT-local. It is intentionally independent from the
// LoRa wire protocol and does not change Config::LORA_PROTOCOL_VERSION.
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint16_t DESCRIPTOR_MAGIC = 0x5344; // "SD"
constexpr uint8_t DESCRIPTOR_REQUEST_LIST = 0x01;
constexpr uint8_t MAX_NAME_BYTES = 24;
constexpr uint8_t MAX_UNIT_BYTES = 12;
constexpr uint8_t MAX_NODE_NAME_BYTES = 24;
constexpr uint32_t FIRMWARE_VERSION = 1;
constexpr char COMMAND_UUID[] = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000005";
constexpr char COMMAND_RESPONSE_UUID[] = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000006";
constexpr uint8_t COMMAND_SET_SAMPLING_PERIOD = 1;
constexpr uint8_t COMMAND_REQUEST_DESCRIPTOR_REFRESH = 2;
constexpr uint8_t COMMAND_REQUEST_SENSOR_RESET = 3;

// Multi-byte fields use little-endian order and IEEE-754 binary32 floats.
// Both ESP32-S3 (gateway) and ESP32-C3 (sensor node) are little-endian
// Xtensa/RISC-V. The wire format is explicitly little-endian and does not
// assume the host CPU endianness beyond byte order.
// Custom UUIDs are reserved for FieldRadio BLE sensor nodes. A node must
// advertise SERVICE_UUID before the gateway will attempt a connection.
constexpr char SERVICE_UUID[] = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000001";
constexpr char DESCRIPTOR_REQUEST_UUID[] = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000002";
constexpr char DESCRIPTOR_DATA_UUID[] = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000003";
constexpr char SENSOR_VALUE_UUID[] = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000004";

// GATT layout v1:
//   SERVICE_UUID
//     DESCRIPTOR_REQUEST_UUID : WRITE or WRITE_NR, 3 bytes {version, op, index}
//     DESCRIPTOR_DATA_UUID    : READ, SensorDescriptorResponse (68 bytes)
//     SENSOR_VALUE_UUID       : NOTIFY/INDICATE, SensorValue (20 bytes)
// A descriptor is selected by index through the request characteristic. This
// avoids a large registry read and remains usable with small BLE MTUs because
// standard GATT long-read/Read-Blob semantics can carry the 68-byte response.
// ESP32-C3 has no PSRAM; keep SENSOR_LORA_QUEUE_DEPTH and
// MAX_SENSORS_PER_NODE small on that target.

enum class SensorType : uint8_t {
  GENERIC = 0,
  TEMPERATURE = 1,
  HUMIDITY = 2,
  PRESSURE = 3,
  LIGHT = 4,
  ACCELEROMETER = 5,
  GYROSCOPE = 6,
  BATTERY = 7,
};

enum class SensorDataType : uint8_t {
  UINT8 = 0,
  INT16 = 1,
  UINT16 = 2,
  INT32 = 3,
  UINT32 = 4,
  FLOAT32 = 5,
  BOOL = 6,
};

enum SensorFlags : uint16_t {
  FLAG_ENABLED = 1u << 0,
  FLAG_EVENT_DRIVEN = 1u << 1,
  FLAG_CALIBRATED = 1u << 2,
  FLAG_READ_ONLY = 1u << 3,
  FLAG_HAS_SOURCE_SEQUENCE = 1u << 4,
  FLAG_DEGRADED = 1u << 5,
  FLAG_HAS_SCHEMA_VERSION = 1u << 6,
};

// If timestamp is zero at gateway receipt, the gateway must replace it with
// its receipt/processing wall-clock time and set QUALITY_TIMESTAMP_GATEWAY.
enum SensorQuality : uint8_t {
  QUALITY_VALID = 0,
  QUALITY_STALE = 1u << 0,
  QUALITY_OUT_OF_RANGE = 1u << 1,
  QUALITY_TIMESTAMP_GATEWAY = 1u << 2,
  QUALITY_TIMESTAMP_INVALID = 1u << 3,
  QUALITY_LINK_DEGRADED = 1u << 4,
};

struct BleAddress {
  uint8_t bytes[6] = {};
  uint8_t type = 0;

  bool operator==(const BleAddress& other) const {
    return type == other.type && std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
  }
  bool operator!=(const BleAddress& other) const { return !(*this == other); }
};

#pragma pack(push, 1)
struct SensorDescriptor {
  uint16_t id = 0;
  uint8_t type = static_cast<uint8_t>(SensorType::GENERIC);
  char name[MAX_NAME_BYTES] = {};
  char unit[MAX_UNIT_BYTES] = {};
  uint8_t datatype = static_cast<uint8_t>(SensorDataType::FLOAT32);
  float scale = 1.0f;
  float offset = 0.0f;
  float min = 0.0f;
  float max = 0.0f;
  uint32_t periodMs = 1000;
  uint16_t flags = FLAG_ENABLED;
  uint8_t schemaVersion = 0;
};

struct SensorDescriptorResponse {
  uint16_t magic = DESCRIPTOR_MAGIC;
  uint8_t version = PROTOCOL_VERSION;
  uint8_t index = 0;
  uint8_t total = 0;
  SensorDescriptor descriptor{};
};

// SensorValue.value is the engineering value after the peripheral has applied
// its descriptor scale/offset. The descriptor retains scale/offset so a gateway
// or future raw-value transport can reproduce the conversion contract.
struct SensorValue {
  uint16_t id = 0;
  float value = 0.0f;
  uint64_t timestamp = 0;
  uint8_t quality = QUALITY_VALID;
  uint8_t flags = 0;
  uint32_t sourceSequence = 0;
};
struct CommandRequest {
  uint8_t commandId = 0;
  uint32_t sequence = 0;
  uint16_t sensorId = 0;
  uint32_t argument = 0;
};

struct CommandResponse {
  uint8_t commandId = 0;
  uint32_t sequence = 0;
  uint8_t result = 0;
  uint16_t errorCode = 0;
};
#pragma pack(pop)

static_assert(sizeof(SensorDescriptor) == 63, "SensorDescriptor wire layout changed");
static_assert(sizeof(SensorDescriptorResponse) == 68, "SensorDescriptorResponse wire layout changed");
static_assert(sizeof(SensorValue) == 20, "SensorValue wire layout changed");
static_assert(std::is_trivially_copyable<SensorDescriptor>::value, "SensorDescriptor must be wire-copyable");
static_assert(std::is_trivially_copyable<SensorValue>::value, "SensorValue must be wire-copyable");

inline bool validDescriptor(const SensorDescriptor& descriptor) {
  if (descriptor.id == 0 || descriptor.periodMs == 0) return false;
  if (descriptor.datatype > static_cast<uint8_t>(SensorDataType::BOOL)) return false;
  if (descriptor.type > static_cast<uint8_t>(SensorType::BATTERY)) return false;
  if (!std::isfinite(descriptor.scale) || !std::isfinite(descriptor.offset) ||
      !std::isfinite(descriptor.min) || !std::isfinite(descriptor.max) ||
      descriptor.min > descriptor.max) return false;
  if (descriptor.name[0] == '\0') return false;
  if (descriptor.name[MAX_NAME_BYTES - 1] != '\0' ||
      descriptor.unit[MAX_UNIT_BYTES - 1] != '\0') return false;
  return true;
}

inline bool validValue(const SensorValue& value) {
  return value.id != 0 && std::isfinite(value.value);
}

} // namespace SensorProtocol
