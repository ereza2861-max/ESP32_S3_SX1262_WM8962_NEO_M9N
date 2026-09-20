# Repository audit — 2026-09-17

## Scope

Static review of the supplied repository for ESP32-S3-WROOM-1-N16R8 / PlatformIO:
logic, robustness, security, feature completeness, GPIO/documentation consistency, and
repository workflow. The bundled `.pio` build directory was excluded from source review.

## Findings

### 1. BLE provisioning scaffold was replaced by the BLE Sensor Reader

The old provisioning facade has been replaced by `BleSensorReader` + `SensorReader`, which
implements the NimBLE GATT central role, dynamic descriptor discovery, notifications,
pairing, validation, and forwarding queues. The remaining validation requirement is a
real BLE GATT sensor node; legacy Bluetooth transport is not compatible.

### 2. BLE is not the only incomplete/scaffold area

The repository contains:
- BLE Sensor Reader: implemented; hardware interoperability testing remains.
- Wi-Fi STA: partial manager/reconnect implementation without an end-to-end provisioning UI/workflow.
- MQTT: production security provisioning from FASE 3 is now present; deployment still depends on the
  documented credential/TLS provisioning workflow.
- X25519/ECDH key rotation: runtime lifecycle is implemented in the current source; target build/interoperability and HIL evidence remain pending.
- Full selective-repeat/SACK fragmentation semantics: still incomplete.
- Secure Boot/flash-encryption production provisioning: FASE 5 manufacturing workflow is being added;
  it remains intentionally explicit and does not burn eFuses during a normal development build.
- HIL/factory/fuzz/native unit testing: incomplete.

The audit therefore does **not** classify BLE as the sole remaining scaffold.

### 3. MQTT TLS verification gate

`MqttClientManager` uses a TLS-capable client when `mqttTlsRequired` is enabled and loads the
provisioned broker CA before connecting. The remaining production gate is credential rotation:
`mqttCredentialRotationDays` is policy state, not by itself a broker credential-rotation protocol.

### 4. GPIO/PCB status language was stronger than the actual project state — normalized

The repository used phrases such as `source of truth Rev-C`, `supplied PCB`, and field/production
language even though the project status is pre-fabrication. The patch establishes
`docs/PROJECT_STATUS.md` as the status baseline and clarifies that Rev-C is a candidate
routing/design contract. Pin rolling remains acceptable until PCB fabrication is explicitly recorded.

### 5. No evidence of hardware validation was found

The routing matrix and codec documents describe test procedures/contracts. They are not execution
records. The repository should not claim RF, audio, PPS, I2C, EMC, or deep-sleep validation until
those tests are performed on actual hardware.

## Validation limitation

PlatformIO CLI is not available in the audit environment, so a clean ESP32-S3 compilation could
not be executed here. The patch therefore avoids claiming a successful firmware build. After
applying the patch on a development machine, run `make ci-build` (or `make build` after local
provisioning) and the repository preflight.

## Recommended next gates

1. Freeze the pin map only when fabrication is explicitly approved/recorded.
2. Validate the BLE Sensor Reader against the real ESP32-C3 BLE-GATT sensor-node implementation.
3. Complete STA/MQTT provisioning and TLS trust handling before treating MQTT as production-ready.
4. Add hardware-backed integration tests for PPS, I2C gauge, radio wake, audio clocks and sleep/wake.
5. Keep Secure Boot/flash encryption as an explicit manufacturing procedure with recovery/update policy.


## Follow-up audit findings — 2026-09-20

### 6. BLE queue drop accounting and cross-task state
The BLE sensor queue previously incremented `sensorDropped` twice when a
`DROP_NEWEST` sample was rejected, and could also count a successful
`DROP_OLDEST` replacement as two drops. The counter now represents actual
discarded samples. Queue policy and WebUI-triggered forget/refresh requests are
also synchronized with atomics because they cross FreeRTOS task boundaries.

### 7. BLE security configuration consistency
`SENSOR_REQUIRE_ENCRYPTION=1` with `BLE_PAIRING_ENABLED=0` is incompatible with
the current sensor-node contract because the node requires an encrypted and
authenticated connection. The reader now rejects that configuration explicitly
instead of attempting an incompatible security fallback.

### 8. BLE health monitoring
The health task previously monitored five core tasks but omitted the BLE sensor
reader and sensor-forwarding tasks even though both are long-lived FreeRTOS
tasks. Their heartbeats are now included in the stalled-task mask.

### 9. Sensor-node serial provisioning
The ESP32-C3 example accepted partially numeric serial input through `String::toInt()`
and reported NVS provisioning success without checking write results. Integer
parsing is now strict and the save path verifies each NVS write.

### 10. Remaining delivery semantics
BLE samples are still best-effort across the downstream MQTT/LoRa boundary.
The current patch does not silently invent persistent store-and-forward semantics;
see GAP E in `DECISIONS.md` for the required product decision.

### 11. Configuration ownership and persistence closure — 2026-09-21
Runtime configuration mutation has been moved behind a dedicated Configuration Manager task and
transactional command queue. Direct runtime `gConfig` writers were removed from WebUI, audio,
LoRaWAN hardware-identity handling and physical-mode paths. Persistence now uses two alternating
NVS slots with generation, CRC and a post-write commit marker; legacy configuration is retained
only as a boot-time migration fallback.

### 12. Build-stack gate remains unresolved
The repository declares PlatformIO 6.13.0 with Arduino + ESP-IDF and also selects the
`esp32_idf5_https_server_compat` fork. PlatformIO 6.13.0's Arduino 2.0.17 stack is based on
ESP-IDF 4.4.7, so the repository does not currently contain evidence for a compatible IDF-5
HTTPS dependency stack. This is an architectural build decision, not a safe version guess.
