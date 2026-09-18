# Phase 1 verification

## GAP-1 — ESP32-C3 sensor-node firmware

Expected:

```text
cd sensor_node_esp32c3
pio run
```

Expected result: `SUCCESS` and no compile/link errors.

Flash and open the console:

```text
pio run -t upload
pio device monitor -b 115200
```

Expected startup evidence:

```text
FieldRadio ESP32-C3 BLE Sensor Node
BLE sensor node ready: <node-name>, passkey <6 digits>
```

## GATT verification

Use a BLE inspector or the gateway and verify the advertised service UUID:

```text
7f2a0000-7b2a-4a6e-9a9f-1b7f7e000001
```

Characteristics:

```text
7f2a0000-7b2a-4a6e-9a9f-1b7f7e000002  WRITE / WRITE_NR
7f2a0000-7b2a-4a6e-9a9f-1b7f7e000003  READ
7f2a0000-7b2a-4a6e-9a9f-1b7f7e000004  NOTIFY
```

The descriptor request is exactly 3 bytes. Request index 0, then read the
67-byte descriptor response. Repeat through `total - 1`.

## Security verification

On first access to an encrypted characteristic:

1. Pair with the six-digit passkey printed by the node.
2. Verify bonding is retained after reconnect/reboot.
3. Verify an unpaired client cannot read the encrypted descriptor characteristic
   or write the encrypted descriptor-request characteristic.
4. Verify the serial console reports authentication status.

The node uses bonding + MITM + Secure Connections and `DISPLAY_ONLY` IO
capability.

## Registry verification

The local registry must reject:

- descriptor ID 0;
- invalid descriptor names/units;
- more than eight descriptors;
- NaN sensor values;
- updates for unknown sensor IDs.

The host-side registry compile check used during this delivery passed.

## Shared wire contract verification

From the gateway repository root:

```text
pio test -e native -f test_sensor_registry
pio test -e native -f test_ble_sensor_reader
```

Expected: both existing native tests pass.

The audit runtime also compiled the existing registry test directly with
`g++` and the shared protocol wrapper; the registry test passed.

## Protocol sizes

Expected compile-time sizes:

```text
SensorDescriptor          62 bytes
SensorDescriptorResponse  67 bytes
SensorValue               15 bytes
```

## Phase boundary

This phase does not implement WebUI inventory, HIL tests, MQTT production
hardening, X25519 rotation, or manufacturing Secure Boot/Flash Encryption
procedures. Those remain later phases as requested.
