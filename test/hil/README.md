# FieldRadio FASE 2 — Hardware-in-the-Loop

This directory contains real hardware regression tests for the ESP32-S3 gateway,
the ESP32-C3 BLE sensor node, a local Mosquitto broker, and (optionally) a LoRa
sniffer/injector.

The tests intentionally **fail**, rather than skip, when a required physical
fixture or control interface is missing.

## Hardware

Minimum BLE setup:

- 1x FieldRadio ESP32-S3 gateway.
- 1x ESP32-C3 sensor node.
- USB serial connection to each board.
- BME280 connected to the C3 if the default 5-sensor profile is used.
- Common ground for external lab power/control equipment.

For the default sensor node, see `sensor_node_esp32c3/README.md`:
BME280 SDA=GPIO8, SCL=GPIO9, battery ADC=GPIO4, digital input=GPIO5.

For LoRa P2P tests:

- 2x FieldRadio gateways with compatible SX1262 hardware.
- Identical authenticated LoRa configuration/key.
- RF connection/antenna appropriate to the hardware and legal test setup.
- Optional raw-frame injection/sniffer fixture for replay testing.

## Software

Install:

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r test/hil/requirements.txt
```

Install and start Mosquitto locally, normally on `127.0.0.1:1883`.

Build/flash the gateway and sensor node with PlatformIO before running tests.

## Required environment

BLE:

```bash
export FIELDRADIO_SENSOR_NAME=FieldRadio-Sensor-C3
export FIELDRADIO_SENSOR_ADDRESS=AA:BB:CC:DD:EE:FF
export FIELDRADIO_SENSOR_PASSKEY=123456
export FIELDRADIO_BLE_PAIR_COMMAND='YOUR_REAL_HOST_PAIR_COMMAND {address} {passkey}'
```

The pairing command must perform an actual host Bluetooth pairing operation.
The test does not fake passkey entry. Bleak delegates passkey handling to the
OS Bluetooth backend and there is no portable cross-platform numeric passkey
callback.

Gateway:

```bash
export FIELDRADIO_GATEWAY_URL=https://192.168.4.1
export FIELDRADIO_WEB_USER=admin
export FIELDRADIO_WEB_PASSWORD='...'
export FIELDRADIO_TLS_VERIFY=0
```

Sensor serial:

```bash
export FIELDRADIO_GATEWAY_SERIAL=/dev/ttyACM0
export FIELDRADIO_SENSOR_SERIAL=/dev/ttyACM1
```

MQTT:

```bash
export FIELDRADIO_MQTT_HOST=127.0.0.1
export FIELDRADIO_MQTT_PORT=1883
export FIELDRADIO_MQTT_TOPIC_ROOT=fieldradio
export FIELDRADIO_GATEWAY_DEVICE_ID=ESP32S3_VOICE_NODE_01
```

For LoRa P2P:

```bash
export FIELDRADIO_GATEWAY_A_URL=https://192.168.4.1
export FIELDRADIO_GATEWAY_B_URL=https://192.168.4.2
```

Replay injection additionally requires:

```bash
export FIELDRADIO_LORA_INJECT_COMMAND='your-lab-injector --gateway {gateway} --seq {seq} --hex {hex}'
export FIELDRADIO_LORA_REPLAY_SEQ=1234
export FIELDRADIO_LORA_REPLAY_FRAME_HEX='...'
```

The injector must transmit the exact authenticated frame twice. A generic radio
sniffer cannot prove replay rejection unless it can reproduce a valid frame.

Node-loss testing additionally requires a real lab power/relay control:

```bash
export FIELDRADIO_SENSOR_POWER_OFF_COMMAND='your-relay-tool off --address {address}'
```

## Running

BLE suite:

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py
```

LoRa suite:

```bash
pytest -v -m hil test/hil/hil_lora_p2p.py
```

Run everything:

```bash
pytest -v -m hil test/hil
```

## Expected behavior

### BLE

- Correct passkey pairing succeeds and the encrypted GATT service is reachable.
- Three wrong passkey attempts all fail; another immediate attempt remains blocked.
- Disconnect/reconnect succeeds.
- All advertised descriptors are requested and read.
- Power loss removes the node after `SENSOR_NODE_EVICTION_MS` plus the test grace.
- Changing the C3 example sensor count from 3 to 5 and forcing refresh causes the
  gateway to rediscover the exact descriptor count.
- Real notifications eventually produce the expected MQTT JSON payload.

### LoRa

- A text sent through gateway A arrives at B.
- With ECDH always compiled and `ecdhRekeyPolicy=1`, both gateways report active ECDH
  state, a text round-trip is observed as wire V5, and the peer survives reboot
  with a freshly regenerated ephemeral key.
- The deep-sleep test arms SX1262 duty-cycle RX, loses an intentionally
  transmitted packet while the receiver sleeps, wakes on a later authenticated
  beacon, and verifies replay state survived the sleep cycle.
- A > MTU text is reassembled intact.
- A raw authenticated frame transmitted twice is identified as replay/duplicate
  by the receiver.
- SOS reaches the peer and gateway A reports `acked=true`.

For ECDH bring-up, build **both** gateway fixtures with
The test suite uses the authenticated
`/api/ecdh/status` endpoint and `/api/deep-sleep` control endpoint; both are
protected by the normal web authentication and CSRF checks.

## Important limitation discovered during Phase 2

The current gateway firmware has a public serial console for status/config/LoRaWAN,
but it does **not** expose a raw P2P LoRa packet injection command. Therefore
replay testing cannot be made truthful using only the existing gateway serial
console. The test requires the explicit lab injector above and fails clearly if it
is absent.

Likewise, Bleak's pairing API is host-backend dependent for passkey entry, so the
correct/wrong passkey tests require a real host pairing helper rather than an
invented Python callback.

These are deliberate test prerequisites, not skipped tests.
