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
The active Espressif32 platform baseline is pioarduino `55.03.39`, resolving
Arduino-ESP32 `3.3.9` and ESP-IDF `5.5.4`. Historical references to the former
`6.13.0` baseline must not be treated as active configuration.


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
**exponential backoff**. Sensor MQTT delivery uses QoS 1 with a broker PUBACK gate before
the MQTT delivery bit is marked complete. A local transport write/publish result alone is
not accepted as broker-authoritative delivery; a missing or mismatched PUBACK leaves the
spool record pending and forces MQTT reconnect.

## GAP M — HIL automation — DECISION LOCKED
Use a **full automated hardware rack** for factory/HIL acceptance. Host-only tests are not
allowed to claim RF, audio, PPS, sleep/wake, power-loss, or provisioning acceptance.

## GAP N — ECDH lifecycle — DECISION LOCKED
Use a **protocol Hybrid** with **Epoch + authenticated beacon** rekey triggers and bounded
retention of the previous key. The current repository compiles ECDH support by default but
keeps the feature disabled at runtime until explicitly selected by `ecdh_rekey_policy`.
Enabling the policy is not a production acceptance criterion by itself; multi-node
interoperability and hardware validation remain required.

## D-02 — configuration ownership — FINAL
A dedicated Configuration Manager task is the sole owner of runtime `gConfig` mutation.
Callers create validated candidates from snapshots and submit them through the transactional
configuration queue with a generation check. Direct cross-task mutation is not permitted.
Persistence is performed by the manager before the new snapshot is published.

## Atomic NVS — FINAL
Runtime configuration is persisted as two alternating NVS slots. Each slot contains a schema,
generation, complete typed payload and CRC. A separate commit marker is written only after the
slot has been written and read back successfully. Recovery selects the newest valid committed
slot; an incomplete/corrupt slot is ignored in favour of the other slot.

## BD-001 — Build-stack decision — FINAL
Use the pioarduino PlatformIO platform with an Arduino core based on ESP-IDF 5.x.
The active environments use pioarduino `55.03.39`, which corresponds to Arduino-ESP32
3.3.9 / ESP-IDF 5.5.4. The HTTPS dependency remains the
`jackjansen/esp32_idf5_https_server_compat` fork because it explicitly targets the
ESP-IDF 5.x family.

The repository must still record target build, link, startup, and TLS handshake evidence
separately; unavailable hardware or dependency-resolution evidence is `NOT VERIFIED`, not PASS.

## MQTT-001 — MQTT credential rotation — FINAL
Use a PKI/certificate-based MQTT credential design. Automatic certificate rotation is
deferred until the formal MQTT protocol specification is complete and approved. The
existing username/password provisioning path is therefore not treated as proof of the
final PKI design and must not be represented as production-complete PKI support.


## Phase A decision closure — QnA.txt

The following decisions are user-selected and must be treated as locked for the
implementation phase:

- D-01 = C — authenticated challenge/response for privileged serial commands.
- D-02 = A — mutex-protected `RuntimeConfig` snapshots for all readers.
- D-03 = A — MQTT QoS 1 with PUBACK as the broker-authoritative delivery gate.

D-01 is implemented in the production serial dispatcher by challenge/response
using a key derived from the existing LoRa key material. The challenge is
single-use and the authenticated session expires after 60 seconds.

D-02 remains an architectural follow-up where legacy direct `gConfig` readers
still exist; this patch does not claim that those readers have been converted.

D-03 is implemented for sensor-spool MQTT delivery by emitting MQTT QoS 1 directly
on the transport used by PubSubClient and requiring the matching PUBACK before marking
the spool delivery bit. PubSubClient 2.8 remains responsible for the MQTT connection
lifecycle because it does not expose a QoS-1 publish API.


## D-04 — AEC / noise processing — FINAL

Use a dedicated 16 kHz AEC working path while retaining the WM8962/I2S hardware
clock at the existing 44.1 kHz configuration. The microphone capture is resampled
to the ESP-SR AEC rate, the playback reference is independently resampled to the
same 16 kHz domain, and the AEC output is converted to the 8 kHz Codec2 speech
domain. The 44.1 kHz hardware path is not globally reconfigured when AEC is enabled.

## D-05 — voice compression — FINAL

Use Codec2 1600 bit/s at 8 kHz speech, one 40 ms codec frame per LoRa voice packet.
The authenticated LoRa envelope remains unchanged; only the voice application
payload changes from PCM/μ-law samples to the fixed 8-byte Codec2 frame. Codec2
1600 produces 64 bits per 40 ms, materially reducing airtime compared with the
previous 160-byte μ-law voice payload.

## D-06 — MQTT PKI enrollment/rotation — FINAL

Use a factory-provisioned, device-unique MQTT client certificate/private key.
The authenticated maintenance provisioning endpoint may replace that certificate
and key during controlled maintenance. The MQTT runtime uses mutual TLS and the
device certificate as the connection authority; username/password MQTT
authentication is no longer selected by the production connection path. The
broker
CA remains separately provisioned. Automatic self-service certificate rotation is
not claimed; replacement is an authenticated maintenance operation.
