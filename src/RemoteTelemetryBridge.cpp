#include "RemoteTelemetryBridge.h"
#include "PersistentConfig.h"
#include "AppState.h"

#include <cstdio>

bool RemoteTelemetryBridge::admitRemoteTelemetry(
    const LoRaManager::RemoteSensorTelemetry& remote,
    SensorSpool& spool) {
  RuntimeConfig config{};
  const bool configAvailable = configSnapshot(config);
  const bool legacy = remote.sourceSequence == 0;
  if (remote.nodeId == 0 || remote.sensorId == 0 || !configAvailable ||
      (legacy && !config.migrationWindowActive)) {
    if (legacy) {
      StateLock lock(gState);
      if (lock.ok()) ++gState.remoteSensorLegacyRejected;
    }
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

  const bool accepted = spool.append(sample, SensorSpool::DELIVERY_MQTT,
                                     SensorSpool::REMOTE_LORA, 1,
                                     remote.sourceSequence, remote.schemaVersion,
                                     remote.firmwareVersion, remote.originNodeId,
                      remote.sourceSequence == 0 ? remote.sampleId : 0);
  if (legacy) {
    StateLock lock(gState);
    if (lock.ok()) {
      if (accepted) ++gState.remoteSensorLegacyAccepted;
      else ++gState.remoteSensorLegacyRejected;
    }
  }
  return accepted;
}
