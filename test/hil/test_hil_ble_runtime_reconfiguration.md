# HIL: BLE runtime reconfiguration

## Purpose

Verify that BLE reader configuration changes through the authenticated WebUI/API
apply to the running gateway without requiring a reboot, and that the persisted
configuration and runtime state remain identical after reconnect/restart.

## Procedure

1. Boot the S3 gateway with a C3 BLE sensor advertising valid telemetry.
2. Record `GET /api/status` and the `runtimeAdvanced` BLE fields.
3. Change `ble_enabled`, scan interval/window, encryption requirement, and pairing
   policy through the authenticated configuration endpoint.
4. Verify the response succeeds and immediately query `GET /api/status` again.
5. Confirm discovery/connection behavior changes without rebooting the gateway.
6. Reboot and verify the same runtime values are restored from NVS.

## Acceptance criteria

- The API response reflects the committed runtime configuration.
- No stale JavaScript literal is used as the post-response default.
- BLE scanning/connection behavior changes at runtime where the setting is
  supported by the running subsystem.
- A reboot restores exactly the same semantic configuration.
- Invalid semantic values are rejected by the shared configuration validator.

TODO(hw): run on a real S3 + C3 pair and capture timestamps for the configuration
commit, BLE stop/start, reconnect, and post-reboot restore.
