#pragma once

#include <cstddef>
#include <cstdint>
#include "ProfileConfig.h"

// FieldRadio BLE sensor node configuration. Keep the defaults conservative:
// the ESP32-C3 has no PSRAM and the node is deliberately independent from the
// gateway's LoRa/Wi-Fi/audio firmware.
namespace SensorNodeConfig {

constexpr char DEFAULT_NODE_NAME[] = "FieldRadio-Sensor-C3";

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SENSOR_SAMPLE_PERIOD_MS = 1000;
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;

// Disabled by default because deep sleep prevents a gateway from maintaining
// a continuous GATT connection. If enabled, the node advertises briefly,
// then sleeps and wakes periodically.
constexpr bool DEEP_SLEEP_ENABLED = false;
constexpr uint32_t DEEP_SLEEP_ADVERTISE_MS = 500;
constexpr uint64_t DEEP_SLEEP_WAKE_US = 5ULL * 1000000ULL;

constexpr bool BLE_SECURITY_ENABLED = true;
constexpr bool BLE_BONDING = true;
constexpr bool BLE_MITM = true;
constexpr bool BLE_SECURE_CONNECTIONS = true;

}  // namespace SensorNodeConfig
