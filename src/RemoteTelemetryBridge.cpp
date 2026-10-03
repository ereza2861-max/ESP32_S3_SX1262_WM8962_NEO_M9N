#include "RemoteTelemetryBridge.h"

#include <cstdio>

bool RemoteTelemetryBridge::admitRemoteTelemetry(
    const LoRaManager::RemoteSensorTelemetry& remote,
    SensorSpool& spool) {
  if (remote.nodeId == 0 || remote.sensorId == 0 ||
      remote.sourceSequence == 0) {
    return false;
  }

  SensorReader::SensorSample sample{};
  sample.nodeId = remote.nodeId;
  sample.sensorId = remote.sensorId;
  sample.value = remote.value;
  sample.quality = remote.quality;
  sample.timestampMs = remote.timestampMs;
  sample.rssi = remote.rssi;
  std::snprintf(sample.nodeName, sizeof(sample.nodeName), "LoRa-remote");
  std::snprintf(sample.sensorName, sizeof(sample.sensorName), "sensor-%u",
                static_cast<unsigned>(remote.sensorId));

  return spool.append(sample, SensorSpool::DELIVERY_MQTT,
                      SensorSpool::REMOTE_LORA, 1,
                      remote.sourceSequence, remote.schemaVersion,
                      remote.firmwareVersion, remote.originNodeId);
}
