#pragma once

#include "LoRaManager.h"
#include "SensorSpool.h"

/**
 * Durable admission boundary for authenticated remote sensor telemetry.
 *
 * The function only succeeds after SensorSpool::append() has accepted the
 * record. The caller owns the subsequent LoRa batch ACK.
 */
class RemoteTelemetryBridge {
public:
  static bool admitRemoteTelemetry(
      const LoRaManager::RemoteSensorTelemetry& remote,
      SensorSpool& spool);
};
