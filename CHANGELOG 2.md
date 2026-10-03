## 2026-10-03 — Durable sensor delivery boundaries and profile SKU binding

- Added `MqttAckRouter` so MQTT sensor spool completion occurs only after a
  matching broker PUBACK.
- Removed the optimistic MQTT `markDelivered()` path from the sensor-forwarding task.
- Added `RemoteTelemetryBridge`; remote LoRa telemetry is ACKed only after durable
  SensorSpool append succeeds, with failed appends returned to the RAM handoff queue.
- Preserved routed `originNodeId` through remote telemetry and SensorSpool persistence.
- Added ESP32-C3 profile SKU binding using a 32-byte SHA-256 `profileBindingTag` in
  NVS `sensor/profile_bind`, with fail-closed boot validation and audit logging.
- Added native contract tests for MQTT PUBACK, LoRa durable ACK, origin propagation,
  and profile binding.

## 2026-10-02 — Audit response

Closed the locked audit-response source defects and functional decisions: native ECDH header boundary, SensorReader config snapshots, cppcheck CI gating, sensor identity binding, batch ACK delivery, C3 calibration/versioning and source-sequence persistence, MRAM sensor deduplication, persist-first BLE commands, ECDH security audit logging, charger-state semantics, placeholder-profile disabling, watchdog maintenance latch, and current six-profile HIL documentation.

# Changelog

## 2026-10-02 — Audit contract fixes
- Raised gateway sensor capacity to 12 and synchronized the registry/queue invariants.
- Persisted SensorSpool schema/firmware metadata in V3 records with V1/V2 compatibility.
- Forwarded source sequence, schema version, and firmware version through existing LoRa telemetry and MQTT JSON.
- Fixed the `sendSensorTelemetry()` 9-argument definition mismatch.
- Updated active BLE documentation/HIL checks to the canonical 68-byte descriptor and 20-byte sensor-value contract.
- Added native/host contract tests for metadata propagation and capacity invariants.


## 2026-09-30 — FRAM → MRAM storage reconstruction
- Migrated ReplayStore from the obsolete I2C FRAM backend to native Everspin MR25H256 SPI MRAM.
- Migrated PersistentConfig runtime storage to MRAM A/B slots with commit-last atomicity and an `MRM1` migration marker.
- Added native `MramStorage` driver at 40 MHz SPI Mode 0.
- Reassigned GPIO42 exclusively to MRAM CS and removed the obsolete GPIO42 TX indicator.
- Retained GPIO39 addressable RGB TX/RX indication.
- Retained NVS as migration source/pre-authority fallback and did not delete NVS data.
- Kept credential-bearing state in encrypted NVS rather than plaintext MRAM.
- Preserved Secure Boot, Flash Encryption, and NVS Encryption.
- Added MRAM migration documentation and storage invariants.
- LoRa V2/V3/V4/V5 framing, fragment/ACK behavior, and ECDH beacon protocol are unchanged.



## 2026-09-19 — G13–G20
- Added dependency/version audit and explicit Espressif32 6.13.0 platform pinning.
- Added per-node BLE passkey provisioning, encrypted peer storage, identity/RPA handling, and sensor queue saturation policy.
- Added ESP32-C3 sensor-node environment, tests, and documentation for the BLE sensor architecture.
