# FASE 2 — Verification

## Static test-harness validation

From repository root:

```bash
python -m py_compile   test/hil/conftest.py   test/hil/hil_sensor_ble.py   test/hil/hil_lora_p2p.py
```

Expected: exit status `0`.

## BLE pairing

Configure:

```bash
export FIELDRADIO_SENSOR_ADDRESS=AA:BB:CC:DD:EE:FF
export FIELDRADIO_SENSOR_PASSKEY=123456
export FIELDRADIO_BLE_PAIR_COMMAND='REAL_PAIR_TOOL {address} {passkey}'
```

Run:

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py::test_pairing_passkey
```

Expected: pairing succeeds with the actual C3 passkey and an encrypted GATT
connection can subsequently be established.

Wrong passkey:

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py::test_pairing_wrong_passkey
```

Expected: all three wrong-passkey attempts fail and the fourth immediate attempt
also fails because the node is expected to enforce its 60-second pairing block.

## Reconnect

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py::test_reconnect_after_drop
```

Expected: a disconnected C3 reconnects successfully.

## Descriptor discovery

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py::test_sensor_discovery
```

Expected:
- service UUID exists;
- descriptor request/data characteristics exist;
- index 0 response is valid;
- every descriptor index through `total-1` is read;
- IDs are non-zero and unique.

## Node eviction

Configure a real power/relay controller:

```bash
export FIELDRADIO_SENSOR_POWER_OFF_COMMAND='REAL_RELAY off --address {address}'
```

Then:

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py::test_node_loss
```

Expected: the node disappears from `/api/sensors/nodes` no later than
`SENSOR_NODE_EVICTION_MS + 30s`.

## Descriptor count change

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py::test_sensor_count_change
```

Expected:
- C3 stages 3 sensors;
- gateway refreshes;
- gateway reports exactly 3 descriptors;
- C3 stages 5 sensors;
- gateway refreshes again;
- gateway reports exactly 5 descriptors.

## MQTT value path

Start Mosquitto, configure the gateway to publish MQTT, then:

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py::test_value_notification
```

Expected a real MQTT message under:

```text
<MQTT_TOPIC_ROOT>/<GATEWAY_DEVICE_ID>/sensor/<node-id>/<sensor-id>
```

Payload must contain:

```text
ts, node, sensor, sensor_name, unit, value, quality, rssi
```

## LoRa encrypted roundtrip

Configure two gateways:

```bash
export FIELDRADIO_GATEWAY_A_URL=https://<gateway-a>
export FIELDRADIO_GATEWAY_B_URL=https://<gateway-b>
```

Run:

```bash
pytest -v -m hil test/hil/hil_lora_p2p.py::test_encrypted_text_roundtrip
```

Expected: a unique test message sent by A appears in B's message history.

## Fragment reassembly

```bash
pytest -v -m hil test/hil/hil_lora_p2p.py::test_fragment_reassembly
```

Expected: a 700+ byte application message arrives intact at B.

## Replay rejection

A real raw-frame injection interface is required:

```bash
export FIELDRADIO_LORA_INJECT_COMMAND='REAL_INJECTOR --gateway {gateway} --seq {seq} --hex {hex}'
export FIELDRADIO_LORA_REPLAY_SEQ=1234
export FIELDRADIO_LORA_REPLAY_FRAME_HEX='<authenticated frame>'
```

Run:

```bash
pytest -v -m hil test/hil/hil_lora_p2p.py::test_replay_rejection
```

Expected: the exact authenticated frame is transmitted twice and receiver logs/
capture show replay or duplicate rejection.

If the injector is absent, the test **fails explicitly**; it does not skip.

## SOS ACK

```bash
pytest -v -m hil test/hil/hil_lora_p2p.py::test_sos_ack_flow
```

Expected: gateway A activates SOS and subsequently reports `acked=true`.

## Phase-2 completion criterion

GAP-2 is considered operationally verified only when the complete configured
hardware suite passes:

```bash
pytest -v -m hil test/hil
```

with no skipped required tests and no missing-prerequisite failures.
