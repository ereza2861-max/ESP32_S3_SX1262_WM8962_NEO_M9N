# HIL: BLE sensor connect

## Procedure
1. Flash gateway and ESP32-C3 sensor node from the same commit.
2. Provision a sensor peer passkey and, when the peer is unbonded with RPA enabled, its 16-byte IRK.
3. Confirm the gateway resolves a bonded peer through NimBLE security and a manually provisioned unbonded RPA through the explicit IRK.
4. Rotate the sensor-node RPA and repeat discovery.

## Acceptance criteria
- Unknown/unresolved RPA is never converted into a new node.
- Bonded peers reconnect without relying on `lastRpa`.
- Notification samples continue after an RPA rotation.
- Authentication failure does not expose or fall back to an unverified passkey.

TODO(hw): verify with a real bonded peer that NimBLE emits the expected identity resolution event during central reconnect.
