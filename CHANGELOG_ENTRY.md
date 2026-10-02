# FieldRadio audit fix — 2026-10-02

- Raised the gateway sensor capacity from 8 to 12 and bound it to `SensorRegistry::MAX_SUPPORTED_SENSORS_PER_NODE`; kept the existing 16-entry LoRa queue unchanged because it already covers a 12-sensor roster.
- Fixed `LoRaManager::sendSensorTelemetry()` to use the existing 9-argument contract and forward source sequence, schema version, and firmware version through the existing 40-byte telemetry payload.
- Upgraded `SensorSpool` persistence to V3 with `schemaVersion` and `firmwareVersion`, while retaining V1 and V2 SD-record migration/read compatibility.
- Forwarded spool metadata and source sequence into the existing MQTT sensor JSON payload as `schema_version`, `firmware_version`, and `source_sequence`; no new MQTT topic was introduced.
- Updated BLE descriptor/value checks and active documentation/HIL references to the canonical 68-byte / 20-byte contract without changing `shared/SensorProtocol.h`.
- Added host/native contract tests for spool metadata round-trip, MQTT JSON metadata, and non-default LoRa telemetry metadata.
- No GPIO/pin mapping, peripheral, dependency, BLE UUID, LoRa wire version/layout, TLS/EST provisioning, or charging indication behavior was changed.
