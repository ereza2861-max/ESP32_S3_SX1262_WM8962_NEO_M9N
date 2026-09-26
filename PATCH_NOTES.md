# FieldRadio GAP close patch notes

## Scope

This patch implements the fixed decisions for GAP-E through GAP-I and the
separate production-security documentation requested in the audit prompt.

### GAP-E — BLE → MQTT/LoRa durability

- Adds `SensorSpool` backed by `/SENSOR/SPOOL.Q`.
- Uses per-record CRC32, append/ACK records, and bounded active state.
- Limits active records to 4096 and the spool file to 1 MiB.
- Evicts the oldest active record when a bound is reached and increments
  `sensorSpoolEvictions`.
- Writes a sample to the spool before downstream publish/transmit.
- Tracks MQTT and LoRa delivery independently so one downstream outage does
  not erase the other obligation.
- Adds deterministic `sampleId = FNV-1a(nodeId, sensorId, timestampMs, value)`
  to MQTT JSON.
- Adds runtime counters:
  `sensorSpoolDepth`, `sensorSpoolEvictions`, `sensorSpoolDrops`,
  `sensorSpoolRecovered`.
- Adds `GET /api/sensors/spool` and `POST /api/sensors/spool/clear`.
- Planned serial/WebUI reboot paths call spool flush/validation.
- A torn final SD record is ignored after reboot; preceding CRC-valid records
  remain recoverable.

### GAP-F — BLE peer-record authenticated integrity

- Adds `BlePeerStore` format v2 with HMAC-SHA256 truncated to 16 bytes.
- Master key is the first 16 bytes of SHA-256 over the ASCII `loraKeyHex`
  followed by `FieldRadio-BLE-Peer-v2`.
- Passkey and last-RPA fields use AES-128-CTR.
- IV is the first 16 bytes of SHA-256(identity || `iv`).
- CRC32 remains as corruption detection, while HMAC provides tamper evidence.
- HMAC is verified before the sensitive fields are decrypted/used.
- Legacy 64-byte v1 records are decrypted using the repository's original
  AES-ECB + FNV-1a CRC format and migrated to v2.
- Adds `peerMacFailures` runtime telemetry.
- Adds `test_ble_peer_integrity.cpp`.

**Wire-layout note:** the prompt's v2 arithmetic is internally inconsistent:
the listed 76-byte fields actually total 74 bytes, and adding the requested
16-byte `irk[16]` makes the concrete layout 90 bytes. The patch therefore
uses the only self-consistent layout that contains every named field:
`PeerRecordV2 == 90` bytes, with `irk[16]` before `mac[16]`. No bytes are
silently overlapped.

### GAP-G — RPA resolution

- Configures NimBLE security with bonding, MITM and Secure Connections.
- For bonded peers, the NimBLE bond/IRK resolver is authoritative; after
  connection the GAP peer identity is obtained from the connection descriptor
  and must match an existing NimBLE bond before a node is accepted.
- For explicitly provisioned unbonded peers, v2 stores a 16-byte IRK and uses
  the Bluetooth Core `ah()` AES-128 operation as fallback resolution.
- Removes the old name-based RPA acceptance path.
- Unknown/unresolved RPA is rejected and never creates a new node.
- Replaces the old `isRpa()` dependency with address-type/RPA-bit handling
  required to select the fallback resolver.
- Adds the Bluetooth Core IRK test vector in `test_ble_rpa_irk.cpp`.
- Adds serial provisioning:
  `ble irk <addr> <32-hex>`.

### GAP-H — ESP32-C3 runtime sensor-driver registry

- Adds `SensorDriver` abstract interface.
- Uses `SensorDriverRegistry` for the immutable source-level profile roster.
- Uses the requested 15-byte `DriverEntry` layout.
- Built-in drivers:
  - BME280 temperature/humidity/pressure channels
  - Battery ADC
  - Digital input
  - Generic I2C placeholder
- Generic I2C validates its configuration but does not perform raw register reads;
  it returns `QUALITY_STALE` until a sensor-specific measurement protocol exists.
- Sensor drivers are instantiated only from the selected immutable profile roster.
- Does not add a Bluetooth Classic/HC-06 bridge.
- Adds `test_sensor_driver_registry.cpp`.
- Corrects the repository sensor-node Makefile target to the actual
  `sensor_node_c3` PlatformIO environment.

### GAP-I — delivery/test gates

- Adds host-only fuzz gate for the common V3/V5 encrypted-frame boundary
  parser, exercising 10,000 random frames under ASan/UBSan.
- The production V3/V5 decrypt paths use the same bounded parser before
  touching frame fields.
- Adds host-only failure-injection state-consistency tests.
- Adds HIL procedures under `test/hil/`.
- Adds `.github/workflows/hil.yml` for host fuzz/failure gates and native tests.
- Updates `DECISIONS.md` to `PARTIALLY CLOSED` with explicit `TODO(hw)` items.

### Production security

- Adds `docs/PRODUCTION_BUILD.md`.
- Adds `make secure-boot-keys`, which only generates manufacturing key
  material and never burns eFuses, flashes hardware, or changes the default
  build environment.
- The target refuses to overwrite existing key files.

## Files added

- `include/SensorSpool.h`
- `src/SensorSpool.cpp`
- `include/BlePeerStore.h`
- `src/BlePeerStore.cpp`
- `shared/EncryptedFrameParser.h`
- `src/EncryptedFrameParser.cpp`
- `sensor_node_esp32c3/src/SensorDriver.h`
- `sensor_node_esp32c3/src/SensorDriverRegistry.h`
- `sensor_node_esp32c3/src/SensorDriverRegistry.cpp`
- `test/test_ble_peer_integrity.cpp`
- `test/test_ble_rpa_irk.cpp`
- `test/test_sensor_driver_registry.cpp`
- `test/test_fuzz_frame_parser.cpp`
- `test/test_failure_injection.cpp`
- `test/hil/test_hil_ble_connect.md`
- `test/hil/test_hil_lora_reconnect.md`
- `test/hil/test_hil_power_loss.md`
- `test/hil/test_hil_factory.md`
- `docs/PRODUCTION_BUILD.md`
- `.github/workflows/hil.yml`
- `PATCH_NOTES.md`

## Files modified

- `include/AppState.h`
- `include/BleSensorReader.h`
- `include/SensorReader.h`
- `include/WebUi.h`
- `src/BleSensorReader.cpp`
- `src/LoRaManager.cpp`
- `src/MqttClientManager.cpp`
- `src/SensorReader.cpp`
- `src/WebUi.cpp`
- `src/main.cpp`
- `platformio.ini`
- `Makefile`
- `DECISIONS.md`
- `sensor_node_esp32c3/src/main.cpp`

## Files deleted

None.

## TODO(hw) / TODO(pin)

- `test/hil/test_hil_ble_connect.md`: real bonded-peer RPA rotation and
  identity-resolution verification.
- `test/hil/test_hil_lora_reconnect.md`: real radio outage and ACK-loss test.
- `test/hil/test_hil_power_loss.md`: SD corruption/torn-write and brownout test.
- `test/hil/test_hil_factory.md`: production eFuse and post-provisioning test.
- No new GPIO number is introduced by the patch. Runtime driver provisioning
  validates GPIOs rather than inventing a board-specific pin map.
- Production Secure Boot V2/Flash Encryption eFuse commands remain outside the
  normal build and are deliberately not automated.

## Verification commands

```sh
git apply --check fieldradio_gap_close.patch
make test
make fuzz
make failure-inject
```

In the audit environment, the host fuzz, failure-injection, peer-integrity,
RPA-vector and sensor-driver-format tests were executed directly and passed.
The full `make test` target requires PlatformIO to be installed.
