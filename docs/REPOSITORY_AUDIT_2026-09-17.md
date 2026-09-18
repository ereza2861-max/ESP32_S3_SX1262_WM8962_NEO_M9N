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
real BLE GATT sensor node; Bluetooth Classic HC-06/SPP is not compatible.

### 2. BLE is not the only incomplete/scaffold area

The repository contains:
- BLE Sensor Reader: implemented; hardware interoperability testing remains.
- Wi-Fi STA: partial manager/reconnect implementation without an end-to-end provisioning UI/workflow.
- MQTT: partial manager with no TLS transport/trust configuration and plaintext credential storage in
  its dedicated NVS namespace.
- X25519/ECDH key rotation: documented but not implemented.
- Full selective-repeat/SACK fragmentation semantics: still incomplete.
- Secure Boot/flash-encryption production provisioning: documented procedure, intentionally not automatic.
- HIL/factory/fuzz/native unit testing: incomplete.

The audit therefore does **not** classify BLE as the sole remaining scaffold.

### 3. MQTT port/security mismatch — retained as a documented gate

`MqttClientManager` uses `WiFiClient`, while the default configured port is `8883`. Port 8883
does not itself enable TLS, so a deployment could incorrectly assume the link is encrypted.
The patch does not silently convert this into `setInsecure()` TLS, because that would provide
encryption without server authentication and could create a false sense of production security.
A production implementation should use a TLS-capable client, explicit CA/server trust policy,
credential provisioning, and a testable failure mode.

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
2. Validate the BLE Sensor Reader against the real ATmega328 + BLE-GATT module sensor-node implementation.
3. Complete STA/MQTT provisioning and TLS trust handling before treating MQTT as production-ready.
4. Add hardware-backed integration tests for PPS, I2C gauge, radio wake, audio clocks and sleep/wake.
5. Keep Secure Boot/flash encryption as an explicit manufacturing procedure with recovery/update policy.
