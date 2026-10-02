# BLE Sensor Node — ESP32-C3 canonical guide

**2026-10-01 contract update:** `SensorDescriptor.periodMs` controls driver
sampling; BLE notifications use the separate fixed one-second reporting
cadence. The node emits zero sensor timestamps and the gateway supplies
wall-clock receipt time. OTA uses a protected AP and authenticated profile
changes; physical cable confirmation remains mandatory.

## Scope and processor selection

The BLE sensor node is a **separate ESP32-C3 DevKitM-1 PlatformIO project**:

```text
sensor_node_esp32c3/
├── platformio.ini
├── include/
│   └── Config.h
├── src/
│   ├── main.cpp
│   ├── BleSensorServer.cpp
│   ├── BleSensorServer.h
│   ├── SensorRegistry.cpp
│   ├── SensorRegistry.h
│   ├── sensors_example.cpp
│   └── sensors_example.h
└── README.md
```

The gateway remains **ESP32-S3-WROOM-1-N16R8**. The sensor node is not an
alternative build environment for the gateway, and `tools/provision.sh` is
intentionally restricted to the ESP32-S3 production environment.

Build the sensor node from the repository root:

```text
make sensor-node-build
```

Or directly:

```text
pio -d sensor_node_esp32c3 run -e esp32-c3-devkitm-1
```

The root GitHub Actions workflow builds both processors. A successful gateway
build therefore no longer hides a broken ESP32-C3 sensor-node build.

## Hardware baseline

| Function | Default GPIO | Constraint |
|---|---:|---|
| BME280 SDA | 8 | strapping pin; fixed by the example |
| BME280 SCL | 9 | strapping pin; fixed by the example |
| Battery ADC | 4 | ADC1; configurable only to GPIO0,1,3,4 |
| Digital/reed input | 5 | configurable through the safe GPIO allow-list |

The ESP32-C3 has ADC1 on GPIO0..4 and ADC2 on GPIO5. GPIO2, GPIO8 and GPIO9
are strapping pins; GPIO12..17 are associated with SPI0/1 flash access; and
GPIO18/19 are USB-JTAG by default. The interactive provisioning commands
therefore reject unsafe GPIO choices rather than accepting arbitrary numbers.

The battery path uses calibrated `analogReadMilliVolts()` with 11 dB attenuation
and then applies `BATTERY_DIVIDER_RATIO`. The divider must still be sized so
the physical ADC input remains within the ESP32-C3 limits.

## Provisioning and persistence

The serial console is available at 115200 baud:

```text
help
show
name FIELD-SENSOR-01
sensors 4
pin battery 4
pin digital 5
save
```

`save` persists the node name, example sensor count, and validated pin map in
NVS and reboots. Persisted pin values are validated again at boot; invalid or
colliding values are replaced with the safe defaults.

The example currently exposes up to **five** descriptors:

1. temperature
2. humidity
3. pressure
4. battery voltage
5. digital/reed state

The registry supports eight descriptors, so adding future sensor drivers does
not require changing the BLE wire contract.

## BLE security contract

NimBLE-Arduino 2.5.1 is configured for bonding, MITM and Secure Connections with
`DISPLAY_ONLY` IO capability. A per-device six-digit passkey is generated and
persisted in NVS.

A client connection is **not considered usable until authentication completes
successfully**. The server starts security immediately on connection and
disconnects peers whose final state is not both encrypted and authenticated.
This matters because the notification characteristic itself uses the normal
`NOTIFY` property; the connection-state gate prevents sensor notifications from
being exposed during or after a failed authentication attempt.

The descriptor request and descriptor read characteristics require encrypted
access. Sensor values are delivered through notifications only after the secure
connection gate is satisfied.

Never expose the serial provisioning console on an unattended production node.
If the passkey is lost during development, erase the node NVS and reprovision
it.

## GATT wire contract

The canonical wire definitions live in `shared/SensorProtocol.h`; the sensor
node must not create a second protocol copy.

- Service: `7f2a0000-7b2a-4a6e-9a9f-1b7f7e000001`
- Descriptor request: WRITE/WRITE_NR, 3 bytes `{version, op, index}`
- Descriptor data: READ, 68 bytes `SensorDescriptorResponse`
- Sensor value: NOTIFY, 20 bytes `SensorValue`

The gateway's BLE Sensor Reader is the corresponding central/GATT client.
The gateway registry supports **12 sensors per sensor node**, matching the
largest existing C3 profile roster; the C3 profiles are not reduced to fit an
older gateway limit.

## Runtime and deep sleep

Continuous GATT connectivity is the default. `DEEP_SLEEP_ENABLED` is `false`.

When deep sleep is explicitly enabled, the node advertises for the configured
window and sleeps when no client is connected. This mode requires gateway-side
reconnect/discovery behavior and is not a transparent replacement for the
continuous-connection mode.

## Verification

Static Python/HIL syntax checks:

```text
python -m py_compile test/hil/conftest.py test/hil/hil_sensor_ble.py test/hil/hil_lora_p2p.py
```

Gateway native regression:

```text
pio test -e native
```

Sensor-node firmware build:

```text
make sensor-node-build
```

BLE HIL tests require real hardware and the external commands documented by
`test/hil/README.md`. They are not evidence of hardware validation until the
configured fixture actually runs them.

## Documentation policy

This file is the canonical operational guide for the ESP32-C3 sensor node.
The former phase-specific `SENSORNODE_APPLY*` and `SENSORNODE_VERIFY*` documents
are retained under `docs/archive/sensor-node/` as historical delivery notes.
They are not operational source-of-truth documents because their instructions
overlap and some contain phase-specific assumptions. Historical audit evidence
belongs in that archive; current operational guidance belongs in this file.
