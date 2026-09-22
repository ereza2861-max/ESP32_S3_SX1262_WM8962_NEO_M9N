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

## PKI-specific power-loss cases

9. During EST enrollment/renewal, cut power after the inactive certificate/key
   slot has been written and verified but before its commit marker is written.
10. Reboot and verify that the previously committed slot remains selected.
11. Repeat with power loss immediately after the new commit marker and before
    MQTT reconnect.
12. Verify that the new certificate/private-key pair is selected only when the
    commit marker and pair validation both succeed.
13. For EST auth mode 2, repeat the first enrollment and verify that the
    bootstrap token is removed from the encrypted NVS configuration after a
    successful enrollment. The token must never appear in serial logs.


## Configuration transaction recovery

For the centralized configuration transaction path, repeat the power interruption
matrix at these durable boundaries:

1. after the pending transaction marker is written and before candidate A/B
   commit;
2. after candidate commit and before subsystem apply returns;
3. during subsystem failure rollback;
4. after persisted rollback and before the transaction marker is cleared.

After reboot, verify that the previous committed generation is restored when the
candidate generation is marked incomplete, and that the transaction marker is
cleared after recovery. No host-only test may be used as evidence for this
physical power-loss acceptance.
