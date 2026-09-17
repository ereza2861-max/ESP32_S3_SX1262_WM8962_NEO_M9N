#include "MqttClientManager.h"

bool MqttClientManager::connect(const String&, uint16_t,
                                const String&, const String&) {
  // TODO: use namespace "mqtt" with NVS encryption and a bounded MQTT client.
  connected_ = false;
  return false;
}

bool MqttClientManager::publish(const String&, const String&) {
  // TODO: integrate telemetry publishing without blocking radio/audio tasks.
  return false;
}

void MqttClientManager::task() {
  // TODO: MQTT reconnect/publish service.
}
