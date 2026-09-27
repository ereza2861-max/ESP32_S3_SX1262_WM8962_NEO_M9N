# HIL: LoRa runtime reconfiguration

## Purpose

Verify that authenticated LoRa runtime configuration changes are applied through
the normal transaction path, including rollback on a failed runtime apply.

## Procedure

1. Boot two S3 gateways with a known-good LoRa link.
2. Record `GET /api/status` and `GET /api/lorawan/status` where applicable.
3. Change ADR/HOP/profile and the applicable LoRaWAN runtime fields through the
   authenticated API.
4. Confirm traffic continues after the runtime apply and that the peer observes
   the new radio behavior.
5. Inject an invalid semantic configuration and confirm it is rejected before
   persistence.
6. Force a runtime-apply failure and verify both runtime and NVS configuration
   roll back to the previous generation.
7. Reboot and verify the accepted configuration is restored.

## Acceptance criteria

- Configuration generation changes only after the semantic validator accepts the
  candidate.
- Successful changes are applied without an unnecessary reboot.
- Failed runtime application leaves the previous runtime and persisted values
  intact.
- ECDH remains compiled in regardless of build flags; `ecdhRekeyPolicy` alone
  controls whether the runtime uses ECDH rekey framing.

TODO(hw): capture packet traces and configuration-generation/audit logs from two
physical S3 nodes.
