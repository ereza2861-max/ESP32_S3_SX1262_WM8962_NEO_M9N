# Phase 1 apply instructions

This delivery intentionally separates the new files from `PATCH.diff`.

## 1. Apply existing-file changes

From the repository root:

```text
git apply PATCH.diff
```

The patch changes only:

- `include/Config.h`
- `include/SensorProtocol.h`
- `platformio.ini`

## 2. Copy the new files

Copy the contents of `new-files/` into the repository root, preserving paths:

```text
new-files/shared/SensorProtocol.h                         -> shared/SensorProtocol.h
new-files/sensor_node_esp32c3/platformio.ini             -> sensor_node_esp32c3/platformio.ini
new-files/sensor_node_esp32c3/include/Config.h           -> sensor_node_esp32c3/include/Config.h
new-files/sensor_node_esp32c3/src/main.cpp               -> sensor_node_esp32c3/src/main.cpp
new-files/sensor_node_esp32c3/src/BleSensorServer.h     -> sensor_node_esp32c3/src/BleSensorServer.h
new-files/sensor_node_esp32c3/src/BleSensorServer.cpp   -> sensor_node_esp32c3/src/BleSensorServer.cpp
new-files/sensor_node_esp32c3/src/SensorRegistry.h      -> sensor_node_esp32c3/src/SensorRegistry.h
new-files/sensor_node_esp32c3/src/SensorRegistry.cpp    -> sensor_node_esp32c3/src/SensorRegistry.cpp
new-files/sensor_node_esp32c3/src/sensors_example.h     -> sensor_node_esp32c3/src/sensors_example.h
new-files/sensor_node_esp32c3/src/sensors_example.cpp   -> sensor_node_esp32c3/src/sensors_example.cpp
new-files/sensor_node_esp32c3/README.md                  -> sensor_node_esp32c3/README.md
```

After copying, `include/SensorProtocol.h` is only a compatibility forwarding
header; `shared/SensorProtocol.h` is the canonical wire-contract source.

## 3. Build gateway and node

Gateway:

```text
pio run -d .
```

Sensor node:

```text
cd sensor_node_esp32c3
pio run
pio run -t upload
pio device monitor -b 115200
```

The sensor-node PlatformIO project uses NimBLE-Arduino 2.5.1 and the same
canonical `shared/SensorProtocol.h` used by the gateway.

## 4. Provision the example node

At the node serial console:

```text
show
name FIELD-SENSOR-01
sensors 5
pin battery 4
pin digital 5
save
```

Record the six-digit BLE passkey printed by the node.

## API note

The implementation targets the NimBLE-Arduino 2.5.1 APIs for security,
server callbacks, encrypted characteristic properties, advertising, and
2.x callback signatures. The exact API surface was checked against the
upstream 2.5.1 documentation before preparing this phase.

## Limitation of this delivery environment

PlatformIO is not installed in the audit runtime, so a real ESP32-C3 compile
was not executed here. Native compilation of the shared protocol and both
registry implementations was checked; the hardware/NimBLE build must be run
with PlatformIO before flashing a physical node.
