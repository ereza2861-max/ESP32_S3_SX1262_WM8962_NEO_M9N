# FieldRadio decisions — GAP A–I

## GAP A — per-node passkey
Each BLE sensor node has its own six-digit passkey. The gateway stores identity-address → passkey mappings in the encrypted `ble_peer` NVS namespace and requires a provisioned passkey before secure connection.

## GAP B — ESP32-C3 sensor node
The supported sensor-node architecture is ESP32-C3 + NimBLE GATT. Legacy Classic-Bluetooth/AVR sensor-node references are removed from active documentation and tests.

## GAP C — BLE identity
The sensor node uses a stable public address. The gateway registry uses identity address as primary key and can retain the last RPA. Unknown RPAs are rejected rather than becoming new registry entries.

## GAP D — sensor queue saturation
The queue remains depth 16. `DROP_OLDEST` is the default; `DROP_NEWEST` is available at runtime. Dropped samples are counted and exposed through runtime status and WebUI.

## Pinning
The Espressif32 platform remains exactly `6.13.0`. Exact versions not provable from the repository are deliberately left as explicit TODO(pin) items rather than guessed.


## GAP E — BLE sensor forwarding durability — CLOSED
Use an SD-backed append-only spool at `/SENSOR/SPOOL.Q`, bounded to 4096 active
records or 1 MiB. Samples are written before downstream I/O, and MQTT/LoRa
delivery state is tracked independently so one downstream failure does not
erase the other delivery obligation. The MQTT payload carries a deterministic
32-bit `sampleId`; LoRa keeps the existing authenticated/deduplication path.

## GAP F — BLE peer-record integrity — CLOSED
Peer records use version 2 with HMAC-SHA256 truncated to 16 bytes, derived from
`SHA-256(loraKeyHex || "FieldRadio-BLE-Peer-v2")`, plus CRC32. Sensitive passkey
and last-RPA fields use AES-128-CTR with an identity-derived IV. Legacy 64-byte
records are accepted only for migration and are rewritten as version 2.

## GAP G — BLE RPA resolution — CLOSED
NimBLE bonding/IRK resolution is the authoritative path for bonded peers. For
explicitly provisioned unbonded peers, a 16-byte IRK can be stored in PeerRecord
v2 and the Bluetooth Core `ah()` AES-128 resolution is used as a fallback.
Unresolved RPAs are rejected and never become new nodes.

## GAP H — sensor-node provisioning model — CLOSED
The supported architecture is ESP32-C3 + BLE GATT without a Bluetooth Classic
bridge. Runtime drivers are registered from versioned NVS configuration in the
`sensor_cfg` namespace. Built-ins are BME280, BatteryAdc, DigitalInput and
GenericI2C; an empty configuration retains the legacy example-sensor fallback.

## GAP I — delivery/test gates — PARTIALLY CLOSED
Host-only fuzzing, failure-injection, peer-integrity tests, RPA vectors and
sensor-driver configuration tests are included in the repository. CI runs the
host-only fuzz/failure gates.

TODO(hw):
- `test/hil/test_hil_ble_connect.md`: verify bonded RPA identity resolution and
  reconnect on real hardware.
- `test/hil/test_hil_lora_reconnect.md`: verify radio outage/ACK-loss behavior.
- `test/hil/test_hil_power_loss.md`: verify SD recovery and brownout behavior.
- `test/hil/test_hil_factory.md`: verify production provisioning and eFuse flow.


## GAP J — transport reliability — CLOSED
Text fragmentation uses **Full Selective Repeat + bounded SACK**. The sender window is 8,
the SACK bitmap is 8 bits, each fragment has an independent retry budget, and reassembly
is hard-bounded to 2048 bytes / 16 fragments / 3 concurrent messages. This is a greenfield
decision; no old-node compatibility branch is required.

## GAP K — Wi-Fi STA lifecycle — DECISION LOCKED
Use **Persistent STA + reconnect + AP fallback**. The AP remains the local recovery path
when STA credentials are absent or the STA link is unavailable. End-to-end credential
provisioning UI remains a delivery gate and must not silently store plaintext credentials
outside the platform's protected configuration store.

## GAP L — MQTT authority and durability — DECISION LOCKED
MQTT is **authoritative** for upstream sensor delivery. The SD spool remains the durability
boundary, with MQTT and LoRa delivery bits tracked independently. Reconnect uses
**exponential backoff**. A future broker-QoS/acknowledgement gate must be validated before
a record is considered broker-authoritatively delivered; a local `publish()` return value
alone is not proof of broker persistence.

## GAP M — HIL automation — DECISION LOCKED
Use a **full automated hardware rack** for factory/HIL acceptance. Host-only tests are not
allowed to claim RF, audio, PPS, sleep/wake, power-loss, or provisioning acceptance.

## GAP N — ECDH lifecycle — DECISION LOCKED
Use a **protocol Hybrid** with **Epoch + authenticated beacon** rekey triggers and bounded
retention of the previous key. The current repository compiles ECDH support by default but
keeps the feature disabled at runtime until explicitly selected by `ecdh_rekey_policy`.
Enabling the policy is not a production acceptance criterion by itself; multi-node
interoperability and hardware validation remain required.

## GAP O — configuration concurrency — DECISION LOCKED
Runtime configuration uses **Mutex + snapshot** semantics. Persistence uses
**transaction + generation counter**, values are **validate-before-persist**, and
propagation uses the same **generation counter** so a task can reject stale snapshots.
Direct cross-task mutation of `gConfig` is a defect to be removed during the configuration
migration; callers must use a snapshot for long-running operations.
