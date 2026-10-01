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
This timestamp is receipt/processing time, not physical measurement time.

## Sensor sampling/reporting

`SensorDescriptor.periodMs` is the authoritative driver sampling cadence.
The sensor-node main loop only provides a scheduler tick. BLE notification is a
separate fixed one-second reporting cadence in protocol v1.

## LoRa sensor telemetry

The gateway always has the local BLE -> SD spool -> MQTT path. It may also emit
`LORA_TYPE_SENSOR_TELEMETRY` as a redundant copy. A receiving gateway consumes
that type and forwards it to MQTT without an application ACK. Packet
authentication/replay protection remains the transport security boundary.

The existing 40-byte sensor telemetry payload uses its pre-existing padding
area for the 32-bit spool `sampleId`; the CRC-covered field layout and
`shared/SensorProtocol.h` BLE wire structs are unchanged.

## OTA/profile security

The ESP32-C3 OTA AP requires the provisioned OTA password and uses a protected
SoftAP. `POST /profile` additionally requires the OTA password and the
time-bounded in-memory session token returned by `/status`, plus the existing
physical-cabling confirmation header.

Secure Boot and flash encryption remain manufacturing/provisioning procedures,
not runtime-enforced features in this patch.
