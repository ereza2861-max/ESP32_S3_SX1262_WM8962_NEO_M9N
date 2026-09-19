# HIL: factory provisioning

## Procedure
1. Start from a blank device NVS/flash state.
2. Run the repository provisioning workflow.
3. Flash gateway and sensor node.
4. Provision a BLE peer, sensor-driver registry, and TLS material.
5. Exercise the first boot and factory reset/reboot paths.

## Acceptance criteria
- No private key/certificate is committed to Git.
- Secure Boot/Flash Encryption remain manufacturing-only operations.
- Runtime sensor-driver configuration survives reboot.
- Legacy example sensors are available when `sensor_cfg` is empty.

TODO(hw): perform eFuse burn and post-provisioning verification on a sacrificial production board.
