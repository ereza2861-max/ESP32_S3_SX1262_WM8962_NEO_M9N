# FieldRadio decisions — GAP A–D

## GAP A — per-node passkey
Each BLE sensor node has its own six-digit passkey. The gateway stores identity-address → passkey mappings in the encrypted `ble_peer` NVS namespace and requires a provisioned passkey before secure connection.

## GAP B — ESP32-C3 sensor node
The supported sensor-node architecture is ESP32-C3 + NimBLE GATT. Legacy Classic-Bluetooth/AVR sensor-node references are removed from active documentation and tests.

## GAP C — BLE identity
The sensor node uses a stable public address. The gateway registry uses identity address as primary key and can retain the last RPA. Unknown RPAs are rejected rather than becoming new registry entries.

## GAP D — sensor queue saturation
The queue remains depth 16. `DROP_OLDEST` is the default; `DROP_NEWEST` is available at runtime. Dropped samples are counted and exposed through runtime status and WebUI.

## Pinning
The Espressif32 platform remains exactly `6.13.0`. Exact versions not provable from the repository are deliberately left as explicit TODO(pin) items rather than guessed.
