# FieldRadio behavioral contract

This file is canonical for the target commit. It supersedes older informal
references to dedicated TX/RX LED nets or blocking feedback.

## RGB status priority

The single addressable RGB LED on `Board::LED_RGB` is the only LED status
contract. Priority is:

1. battery critical — red
2. battery low — orange
3. TX active — blue
4. RX active — green
5. idle — off

RX/TX are represented by the RGB LED; `Board::LED_RX` is not a board symbol.
Charging is represented separately by `Board::BATTERY_CHARGE_ESTIMATE_LED` and
means **charge probable**, never authoritative charger state.

## Feedback scheduling

Buzzer and haptic feedback are queued and serviced from the cooperative main
loop. Calls to the feedback API do not busy-wait and must not contain a
duration loop or `delay()` for the pulse duration.

SOS buzzer playback remains its own non-blocking state machine and takes
precedence over ordinary queued feedback.

## Battery

`updateBattery()` samples the routed ADC fallback at the configured cadence and
updates battery-history, cycle-estimate, charge-probable, and persistence state.
The MAX17048 path remains authoritative for its voltage/SOC fields when the
gauge is available; ADC health bookkeeping is still serviced.

Charging state is heuristic because Rev-C has no charger STAT input.

## Sensor timestamp

The ESP32-C3 sensor node emits `timestamp=0`. The gateway replaces zero or
invalid pre-epoch timestamps with gateway wall-clock time and sets
`QUALITY_TIMESTAMP_GATEWAY` (and `QUALITY_TIMESTAMP_INVALID` when applicable).
This timestamp is receipt/processing time, not physical measurement time. The
gateway must set `QUALITY_TIMESTAMP_GATEWAY` whenever it replaces a zero sensor
timestamp; this rule is independent of transport.

## Sensor sampling/reporting

`SensorDescriptor.periodMs` is the authoritative driver sampling cadence.
The sensor-node main loop computes its tick from the registered descriptors as
`max(SENSOR_SAMPLE_PERIOD_MS, minimum registered periodMs)`. BLE notification is
a separate fixed one-second reporting cadence in protocol v1.

## LoRa sensor telemetry

The gateway always has the local BLE -> SD spool -> MQTT path. It may also emit
`LORA_TYPE_SENSOR_TELEMETRY` as a redundant copy. A receiving gateway consumes
that type and forwards it to MQTT without an application ACK. Packet
authentication/replay protection remains the transport security boundary.

The existing 40-byte sensor telemetry payload uses its pre-existing padding
area for the 32-bit spool `sampleId`, schema version, firmware version, and
source sequence. Zero padding remains the legacy/default representation.

## OTA/profile security

The ESP32-C3 OTA AP requires the provisioned OTA password and uses a protected
SoftAP. `POST /profile` additionally requires the OTA password and the
time-bounded in-memory session token returned by `/status`, plus the existing
physical-cabling confirmation header. Firmware upload requires both
`X-OTA-Session` and the provisioned OTA password at upload start and completion.

Secure Boot and flash encryption remain manufacturing/provisioning procedures,
not runtime-enforced features in this patch.

## BLE sensor commands and source sequencing

The BLE command channel is additive: `COMMAND_UUID` is write/encrypted and
`COMMAND_RESPONSE_UUID` is read/notify/encrypted-gated by the authenticated BLE
connection. Existing descriptor request/data and sensor-value characteristics
remain unchanged. Command sequence numbers are persisted per peer on the
gateway and last accepted sequence numbers are persisted on the sensor node to
reject replayed commands.

Sensor values may carry a source sequence under `FLAG_HAS_SOURCE_SEQUENCE`.
The gateway uses `(sourceId, sourceSequence)` for LoRa telemetry deduplication
when present and falls back to `sampleId` for legacy telemetry.


## Sensor telemetry delivery contract — audit response

Remote sensor telemetry uses authenticated envelope identity as the MQTT topic
identity. A routed application origin is preserved separately as
`origin_node_id` when it differs. Direct telemetry whose payload `nodeId` does
not match the authenticated LoRa source is rejected.

Remote telemetry is acknowledged with `LORA_TYPE_SENSOR_BATCH_ACK`. ACK records
use a bounded bitmap window over `(nodeId, sensorId, sourceSequence)`. The
sender keeps the LoRa delivery bit pending until the authenticated ACK is
consumed from the durable sensor spool path.

## 2026-10-04 delivery invariants
- Remote LoRa telemetry uses `reserve → durable SensorSpool admission → dedup commit → batch ACK`.
- A duplicate source sequence is ACKed only when the matching durable spool record exists.
- LoRa batch ACK records remain pending until `transmitHopped()` succeeds and are mirrored to MRAM.
- MQTT QoS1 waits parse unrelated inbound packets instead of treating them as PUBACK failures.


## Audit decision contract — 2026-10-04

### Origin identity
Remote/routed telemetry carries `originNodeId` separately from the receiving/forwarding
`nodeId`. The sample identity is `(originNodeId, sensorId, sourceSequence)` plus the
gateway spool `sampleId`. Local BLE telemetry keeps `originNodeId=0`. The LoRa decoder
preserves the origin field already present in the 40-byte telemetry payload, and MQTT
forwards it as `origin_node_id`.

### Retry and delivery
MQTT QoS 1 is broker-authoritative: a matching PUBACK is required before the MQTT
delivery bit is marked complete. A missing/mismatched PUBACK leaves the spool record
pending and reconnect uses the configured exponential backoff. After restart, both
`PENDING` and `PENDING_RETRY` journal states are recovered by removing the stale MQTT
packet-id entry while retaining the spool record, so the next publish retries the same
`sampleId`. LoRa ACKs are recovered from MRAM and retried automatically.

### Deduplication
Gateway deduplication is performed before remote admission/ACK and durable sensor
deduplication is keyed by origin/source identity. Backend deduplication remains
required because MQTT retry is at-least-once by design. Unsupported protocol versions
are rejected through explicit compatibility/version checks rather than silently
interpreted as a different schema.

### OneWire sampling
All logical OneWire temperature sensors sharing one physical bus use one
non-blocking conversion per bus cycle. Placeholder drivers remain descriptors with
`QUALITY_STALE` until a production-ready profile is validated.


## Legacy remote telemetry migration

Remote LoRa telemetry with `sourceSequence == 0` is accepted only while
`RuntimeConfig::migrationWindowActive` is true (default true for the migration
release). Use authenticated `POST /api/sensors/legacy-migration` with
`enabled=true|false` to change the runtime flag. The setting is intentionally
runtime-only; it resets to the default after reboot. Legacy acceptance is still
subject to durable spool admission, and accepted/rejected attempts are exposed
by the sensor dedup statistics endpoint. Turn the flag off after the migration
window; do not treat the compatibility path as permanent protocol policy.

## Sensor telemetry CRC versions

SensorTelemetry wire version 1 retains its legacy CRC check for backward
compatibility. Version 2 covers the full timestamp through byte 20 and uses a
22-byte CRC coverage window with the CRC-low storage byte zeroed during
calculation. New serializers emit version 2; decoders continue accepting v1.
