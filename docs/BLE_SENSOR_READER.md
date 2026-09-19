# BLE Sensor Reader

The ESP32-S3 role is **BLE central / GATT client**. It scans for external sensor
nodes, connects to nodes advertising the FieldRadio Sensor Service, discovers a
runtime sensor descriptor list, subscribes to sensor values, and forwards
validated samples to LoRa and MQTT queues.

## Supported sensor-node contract

The node must implement the UUIDs and packed wire structures in
`include/SensorProtocol.h`:

- service: `7f2a0000-7b2a-4a6e-9a9f-1b7f7e000001`
- descriptor request: `...0002`
- descriptor data: `...0003`
- sensor value notify/indicate: `...0004`

The descriptor list is dynamic. A node may expose up to the configured
`SENSOR_MAX_SENSORS_PER_NODE` sensors; the gateway does not assume fixed sensor
IDs, names, units, or values. If a notification arrives for a newly introduced
sensor ID, the gateway requests a reconnect and re-discovers the descriptor list
outside the notification callback.

## Pairing

BLE pairing is controlled by `RuntimeConfig::blePairingEnabled`. Each sensor node
has its own six-digit passkey. The gateway requires that passkey to be provisioned
per identity and stores the identity → passkey mapping in the `ble_peer` NVS
namespace. The passkey is masked in listings; it is not derived from the public
BLE address. NimBLE remains the authoritative bond store; the application does not
duplicate LTK/IRK material in its own NVS namespace.

The NimBLE build is provisioned for three simultaneous connections and eight
stored bonds. `SENSOR_MAX_NODES` is intentionally limited to three or fewer.

## Important hardware compatibility

The supported sensor-node architecture is ESP32-C3 with the repository's NimBLE GATT contract. A sensor node must implement the advertised service and characteristics described above.

## Failure and recovery behavior

- Failed pairings are temporarily blocked after the configured failure count.
- Disconnected sensor nodes are eligible for eviction after
  `SENSOR_NODE_EVICTION_MS`.
- Sensor callbacks only validate and enqueue data; LoRa/MQTT network work is
  performed outside the BLE callback path.
- Invalid descriptors, invalid values, malformed lengths, and out-of-range
  values are rejected or marked in the sample quality field.

## Provisioning parameters

For a normal BLE Sensor Reader build, the deployment must have at least:

1. a valid 32-hex-character `FIELDRADIO_LORA_KEY_HEX`;
2. a unique `FIELDRADIO_DEVICE_ID`;
3. `SENSOR_READER_ENABLED=1`;
4. a sensor node implementing the exact GATT contract above.

If BLE pairing is enabled, the external node must support the selected BLE
pairing/bonding method and use the provisioned per-node six-digit passkey.
