#include <cassert>
#include <cmath>
#include <cstring>
#include "SensorRegistry.h"
#include "SensorTelemetry.h"

namespace {

SensorProtocol::SensorDescriptor makeDescriptor(uint16_t id, const char* name) {
  SensorProtocol::SensorDescriptor d{};
  d.id = id;
  d.type = static_cast<uint8_t>(SensorProtocol::SensorType::TEMPERATURE);
  std::strncpy(d.name, name, sizeof(d.name) - 1);
  std::strncpy(d.unit, "degC", sizeof(d.unit) - 1);
  d.datatype = static_cast<uint8_t>(SensorProtocol::SensorDataType::FLOAT32);
  d.scale = 1.0f;
  d.offset = 0.0f;
  d.min = -40.0f;
  d.max = 125.0f;
  d.periodMs = 1000;
  d.flags = SensorProtocol::FLAG_ENABLED;
  return d;
}

void test_add_and_deduplicate_node() {
  SensorRegistry registry(2, 2);
  SensorProtocol::BleAddress address{{1, 2, 3, 4, 5, 6}, 1};
  size_t index = 99;

  assert(registry.upsertNode(address, "ATmega-01", -55, 10, index));
  assert(index == 0);
  assert(registry.nodeCount() == 1);

  assert(registry.upsertNode(address, "ATmega-01-renamed", -50, 20, index));
  assert(index == 0);
  assert(registry.nodeCount() == 1);
  assert(registry.node(0) != nullptr);
  assert(std::strcmp(registry.node(0)->name, "ATmega-01-renamed") == 0);
  assert(registry.node(0)->rssi == -50);
}

void test_descriptor_and_value_lifecycle() {
  SensorRegistry registry(1, 2);
  SensorProtocol::BleAddress address{{9, 8, 7, 6, 5, 4}, 0};
  size_t node = 0;
  assert(registry.upsertNode(address, "node", -60, 100, node));

  const auto descriptor = makeDescriptor(42, "temperature");
  assert(SensorProtocol::validDescriptor(descriptor));
  assert(registry.upsertDescriptor(node, descriptor));
  assert(registry.sensorCount(node) == 1);
  assert(registry.findSensor(node, 42) == 0);

  SensorProtocol::SensorValue value{};
  value.id = 42;
  value.value = 23.5f;
  value.timestamp = 1700000000000ULL;
  value.quality = SensorProtocol::QUALITY_VALID;
  assert(registry.updateValue(node, value, 200));
  assert(registry.value(node, 0) != nullptr);
  assert(registry.value(node, 0)->value == 23.5f);

  value.id = 999;
  assert(!registry.updateValue(node, value, 300));
}

void test_limits_and_invalid_descriptor() {
  SensorRegistry registry(1, 1);
  SensorProtocol::BleAddress a{{0, 0, 0, 0, 0, 1}, 0};
  SensorProtocol::BleAddress b{{0, 0, 0, 0, 0, 2}, 0};
  size_t index = 0;
  assert(registry.upsertNode(a, "a", -70, 0, index));
  assert(!registry.upsertNode(b, "b", -70, 0, index));

  auto invalid = makeDescriptor(0, "bad");
  assert(!SensorProtocol::validDescriptor(invalid));
  assert(!registry.upsertDescriptor(0, invalid));

  assert(registry.upsertDescriptor(0, makeDescriptor(1, "one")));
  assert(!registry.upsertDescriptor(0, makeDescriptor(2, "two")));
}

void test_eviction_reclaims_disconnected_node() {
  SensorRegistry registry(2, 1);
  SensorProtocol::BleAddress a{{1, 1, 1, 1, 1, 1}, 0};
  SensorProtocol::BleAddress b{{2, 2, 2, 2, 2, 2}, 0};
  SensorProtocol::BleAddress c{{3, 3, 3, 3, 3, 3}, 0};
  size_t index = 0;
  assert(registry.upsertNode(a, "a", -70, 0, index));
  assert(registry.upsertNode(b, "b", -70, 0, index));
  assert(registry.markConnected(0, true, 10));
  assert(registry.evictDisconnected(1000, 100, index));
  assert(registry.nodeCount() == 1);
  assert(registry.findNode(a) == 0);
  assert(registry.upsertNode(c, "c", -70, 1000, index));
  assert(registry.nodeCount() == 2);
  assert(registry.findNode(c) >= 0);
}

void test_connection_state_and_clear() {
  SensorRegistry registry(1, 2);
  SensorProtocol::BleAddress address{{6, 5, 4, 3, 2, 1}, 1};
  size_t node = 0;
  assert(registry.upsertNode(address, "node", -40, 1, node));
  assert(registry.markConnected(node, true, 2));
  assert(registry.node(node)->connected);
  assert(registry.upsertDescriptor(node, makeDescriptor(7, "temp")));
  assert(registry.clearDescriptors(node));
  assert(registry.sensorCount(node) == 0);
  assert(registry.value(node, 0) == nullptr);
  assert(registry.markConnected(node, false, 3));
  assert(!registry.node(node)->connected);
}

void test_sensor_delta_encoding() {
  assert(SensorTelemetry::shouldReportSensor(100.0f, 102.1f, 1000, 2000, 0.02f, 60000));
  assert(!SensorTelemetry::shouldReportSensor(100.0f, 101.0f, 1000, 2000, 0.02f, 60000));
  assert(SensorTelemetry::shouldReportSensor(100.0f, 100.1f, 1000, 62000, 0.02f, 60000));
  assert(SensorTelemetry::shouldReportSensor(NAN, 1.0f, 1000, 2000, 0.02f, 60000));
  assert(SensorTelemetry::shouldReportSensor(0.0f, 0.03f, 1000, 2000, 0.02f, 60000));
}

void test_sensor_lora_payload_serialize() {
  uint8_t payload[SensorTelemetry::PAYLOAD_BYTES] = {};
  const float value = 23.5f;
  assert(SensorTelemetry::serializeSensorTelemetry(payload, 0x11223344UL, 0x5566,
                                                   value, 0xA5, 0x0102030405060708ULL) == 40);
  assert(payload[0] == 0x53);
  assert(payload[1] == 1);
  assert(payload[2] == 0x44 && payload[3] == 0x33 &&
         payload[4] == 0x22 && payload[5] == 0x11);
  assert(payload[6] == 0x66 && payload[7] == 0x55);
  float decoded = 0.0f;
  std::memcpy(&decoded, payload + 8, sizeof(decoded));
  assert(decoded == value);
  assert(payload[12] == 0xA5);
  assert(payload[21] == 0x66 && payload[22] == 0xB8);
  const uint8_t expectedTimestamp[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
  assert(std::memcmp(payload + 13, expectedTimestamp, sizeof(expectedTimestamp)) == 0);
  for (size_t i = 23; i < sizeof(payload); ++i) assert(payload[i] == 0);
  uint8_t payload2[SensorTelemetry::PAYLOAD_BYTES] = {};
  assert(SensorTelemetry::serializeSensorTelemetry(payload2, 0x11223344UL, 0x5566,
                                                   value, 0xA5, 0x0102030405060708ULL) == 40);
  assert(std::memcmp(payload, payload2, sizeof(payload)) == 0);
}


} // namespace

int main() {
  test_add_and_deduplicate_node();
  test_descriptor_and_value_lifecycle();
  test_limits_and_invalid_descriptor();
  test_eviction_reclaims_disconnected_node();
  test_connection_state_and_clear();
  test_sensor_delta_encoding();
  test_sensor_lora_payload_serialize();
  return 0;
}
