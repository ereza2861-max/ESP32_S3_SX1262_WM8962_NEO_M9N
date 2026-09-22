# Certificate NVS A/B power-loss HIL test

This HIL case validates the certificate/key slot commit marker used by D-06.

## Procedure

1. Provision a factory bootstrap certificate and matching private key.
2. Enable `cert_lifecycle_enabled=1` against a lab EST endpoint.
3. Trigger `cert renew` and capture the NVS writes with the power-control fixture.
4. Cut power after the inactive `cert_a|key_a` or `cert_b|key_b` values are written,
   but before `cert_commit_*`.
5. Reboot and verify the previous committed certificate/key is still selected.
6. Repeat with power loss immediately after the commit marker and before MQTT reconnect.
7. Verify the new certificate/key are selected and MQTT reconnects using the new
   certificate.
8. Repeat both cases with the two slots reversed.

## Acceptance

- No boot may select a slot whose commit marker is absent.
- A power loss before the commit marker must not discard the previously committed
  certificate/key.
- A committed slot must contain a certificate/private-key pair that parses and
  matches before MQTT reconnect.
- The device must never print private-key material.
