# Production-security acceptance

## Status

**OPEN — source/build readiness is not manufacturing acceptance.**

This repository is pre-fabrication. No claim is made here that Secure Boot V2,
Flash Encryption, eFuse state, device provisioning, or HIL validation has already
been performed.

## Definition of done

Production-security acceptance is complete only when all of the following are
available for the manufacturing validation sample:

1. A reproducible production build artifact and its SHA-256 hash.
2. Signing evidence identifying the exact build artifact.
3. An eFuse/security-state summary captured from the provisioned device.
4. A provisioning log for the device-specific security and credential workflow.
5. A HIL/manufacturing acceptance report covering the applicable production
   procedures.

The minimum evidence set is stored outside normal source control under:

```text
artifacts/production/
├── build.sha256
├── signing.log
├── efuse-summary.txt
├── provisioning.log
├── hil-report.md
└── manifest.sha256
```

`manifest.sha256` must contain SHA-256 entries for the evidence files and any
production binary/artifact that the reviewer wants the acceptance gate to bind.

## Verification gate

Run:

```text
tools/check-production-acceptance.sh
```

The script only checks that the required evidence exists, is non-empty, and
matches `manifest.sha256`. It does not generate evidence and cannot establish
that a device was actually provisioned or tested.

## Non-claims

The following are intentionally **not** claimed until the evidence exists:

- Secure Boot V2/eFuse provisioning on a physical device;
- Flash Encryption state on a physical device;
- successful production credential provisioning;
- HIL/manufacturing validation;
- PCB or audio/RF hardware validation.

Development and host/native tests may support firmware correctness, but they do
not replace the manufacturing evidence above.


## Q16 — Hybrid acceptance policy

**Q16=C (hybrid)** is the official acceptance policy. Automated production gates
must collect reproducible build/security/test evidence, while hardware-dependent
RF/audio/power/provisioning checks still require the designated manual fixture
procedure and signed evidence. Host/native tests cannot claim HIL acceptance.
