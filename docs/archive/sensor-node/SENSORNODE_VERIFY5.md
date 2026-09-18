# FASE 4 — GAP-4 Verification

## 1. Patch

```bash
git apply --check PATCH4.diff
git apply PATCH4.diff
git diff --check
```

Expected: no errors.

## 2. New files

```bash
test -f shared/LoRaEcdhRekey.h
test -f test/test_lora_ecdh_rekey.cpp
```

Expected: both exist.

## 3. Default feature state

Inspect build configuration:

```text
FIELDRADIO_LORA_ECDH_REKEY_ENABLED=0
```

Expected:
- ECDH disabled;
- existing HMAC rotating-key behavior unchanged;
- existing LoRa V2/V3 interoperability unchanged.

## 4. Native unit test

```bash
pio test -e native
```

Expected:
- all existing native tests pass;
- ECDH scaffold tests pass.

The scaffold test checks:
- exact 70-byte beacon envelope;
- magic/version;
- 32-byte ephemeral key;
- 32-byte static key;
- malformed envelope rejection;
- exact two-period retention boundary.

## 5. Default ESP32-S3 build

```bash
pio run -e esp32-s3-wroom-1
```

Expected: successful build with ECDH disabled.

## 6. No accidental wire activation

Capture a normal neighbor beacon with ECDH disabled.

Expected payload remains the existing 8-byte neighbor beacon payload; no ECDH
key material is broadcast.

## 7. Security acceptance criteria before activation

The feature must NOT be considered complete merely because the scaffold
compiles. Before changing the default to `1`, verify on two physical nodes:

### Key generation/storage
- each node creates a unique X25519 long-term keypair;
- private key survives reboot;
- private key is never logged or transmitted;
- NVS storage is protected by the production flash-security configuration.

### Epoch rollover
- both peers derive the same session key for the same epoch;
- a new ephemeral key is generated at each epoch;
- previous key remains accepted for at most 2 epochs;
- keys older than the retention window are rejected.

### Forward secrecy
- old ephemeral private material is erased;
- compromise of a current ephemeral private key does not expose unrelated
  historical epochs;
- compromise of the long-term private key alone cannot recover past session
  traffic when ephemeral private keys have been erased.

### Authentication
- replacing the peer static public key without authorization is rejected;
- the ECDH beacon remains authenticated by the existing trust mechanism;
- a forged beacon cannot force a chosen session key.

### Failure/fallback
- when ECDH negotiation fails, the documented HMAC-derived legacy key is used;
- fallback is observable in diagnostics;
- fallback cannot silently downgrade an authenticated peer that requires ECDH
  after a future protocol-version negotiation;
- malformed/old/future epoch values are rejected safely.

### Interoperability
Test:
- clean boot;
- reboot during rollover;
- packet loss during rollover;
- 1× and 2× epoch clock skew;
- temporary beacon loss;
- simultaneous transmission;
- replay of an old authenticated ECDH beacon;
- peer replacement.

## Current phase result

This phase intentionally provides the protocol/type/state scaffold and tests
but does **not** claim X25519 operational readiness. The actual crypto API,
NVS key lifecycle, beacon transmission, peer binding, and session-key
integration remain explicit hardware bring-up TODOs.
