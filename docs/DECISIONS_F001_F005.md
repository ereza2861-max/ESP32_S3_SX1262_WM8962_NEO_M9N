# Decision contract — F001..F005

This document records the closure decisions applied by `fieldradio_decision_closure.patch`.
The decisions are additive to `old-patch-artifact.patch`; existing transaction, generation,
range-pair, throttling, and persistence behavior is retained.

## F001 — Configuration ownership

### Q1 — A: Strict snapshot contract
All subsystem readers use `configSnapshot()` and local `RuntimeConfig` values. Direct
`gConfig.` field reads are prohibited outside `PersistentConfig.cpp`/its declaration.

**Consequence:** configuration reads become operation-local and deterministic.

### Q2 — A: Snapshot once per operation
A subsystem operation takes one snapshot and uses that local value for the operation.

**Consequence:** a configuration update cannot produce a mixed read of old/new fields.

### Q3 — A: Existing operation uses old snapshot
An operation already in progress completes with its captured snapshot; later operations
observe the committed configuration.

**Consequence:** runtime behavior follows the transaction boundary instead of changing
halfway through an operation.

### Q4 — A: Remove external direct-reader visibility incrementally
The architecture contract is that external subsystem code must not read `gConfig`
directly. The existing global remains available inside `PersistentConfig.cpp` for
the configuration manager and is not removed in this patch.

**Consequence:** this closes D-02 reader behavior without breaking the established
internal configuration manager API; final symbol removal can be a separate migration.

## F002 — MQTT TLS semantics

### Q1 — C: TLS optional only for development/test; production mandatory
Production is defined by `FIELDRADIO_PRODUCTION_BUILD` or an enabled Secure Boot V2
build profile. Development builds may retain the existing optional runtime flag.

**Consequence:** production cannot intentionally select plaintext MQTT.

### Q2 — B: Retain `mqttTlsRequired`, forced true by policy
The field remains for compatibility. When the production TLS policy is active, a false
value is rejected by runtime apply/begin and by the WebUI.

**Consequence:** existing persistence/API shape is preserved while production policy
is fail-closed.

### Q3 — A: Automatic migration to true
A legacy persisted `mqtt_tls=false` is normalized to true when the mandatory policy is
active and an audit message is emitted.

**Consequence:** old configuration cannot silently request plaintext MQTT in production.

### Q4 — C: Plaintext MQTT only in development builds
The production policy rejects plaintext; development retains the existing capability.

**Consequence:** native/unit tests do not pretend to be WiFi/TLS integration tests.

## F003 — Class-D hardware gate

### Q1 — A: Disabled until hardware evidence exists
`CLASS_D_ENABLED` remains false.

**Consequence:** firmware cannot claim Class-D hardware validation before fabrication.

### Q2 — A: Schematic + netlist + PCB routing + speaker/load specification
All four evidence classes are required before the immutable evidence gate can be enabled.

**Consequence:** software configuration cannot substitute for the physical hardware contract.

### Q3 — A: Immutable compile-time/hardware contract
Class-D enablement and hardware characteristics remain compile-time contract values.

**Consequence:** runtime configuration cannot redefine the hardware topology.

### Q4 — A: Read-only/disabled WebUI control
When Class-D is unavailable, the WebUI disables the control rather than offering a
runtime toggle.

**Consequence:** UI behavior reflects the compile-time hardware capability.

## F004 — Production security acceptance

### Q1 — D: Build + provisioning + HIL/manufacturing evidence
Source/build configuration is not sufficient for final production-security acceptance.

**Consequence:** acceptance remains OPEN until physical evidence is supplied.

### Q2 — C: Manufacturing validation sample
At least the manufacturing validation sample must complete the production-security
workflow before acceptance is closed.

**Consequence:** the repository does not claim that every production unit has already
been provisioned.

### Q3 — D: Complete evidence set
The minimum record includes build hash, signing evidence, eFuse/security-state summary,
provisioning log, and HIL/manufacturing report.

**Consequence:** reviewers can bind acceptance to a reproducible artifact and physical
security state.

### Q4 — A: Source production-ready, acceptance OPEN
The source can contain the production workflow without claiming that manufacturing
acceptance has occurred.

**Consequence:** no fabricated HIL/eFuse/hardware status is introduced.

## F005 — mqttCredentialRotationDays

### Q1 — B: Compatibility metadata
`mqttCredentialRotationDays` remains persisted and exposed for compatibility. It is
explicitly not an automatic credential-rotation trigger.

**Consequence:** no implicit credential lifecycle behavior is introduced; the formal
MQTT PKI lifecycle remains the authority for certificate rotation.

## Additive-fix boundary

The patch does not change the locked constants, existing transaction signatures,
durable transaction journal/recovery, per-IP throttle table/global circuit breaker,
or the established `Config::*` range-pair contract from the accepted baseline.
