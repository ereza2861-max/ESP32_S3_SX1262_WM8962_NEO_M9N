# FASE 4 — GAP-4 Apply

## Scope

This phase is intentionally a **scaffold**. X25519/ECDH is **not active** by
default and existing LoRa packet encryption behavior is unchanged.

### 1. Apply existing-file patch

From repository root:

```bash
git apply PATCH4.diff
git diff --check
```

Existing files modified:

```text
include/Config.h
include/LoRaManager.h
```

### 2. Copy new files

Extract/copy `FASE4_NEW_FILES.zip` into repository root:

```bash
unzip -o FASE4_NEW_FILES.zip -d /path/to/repository
```

This creates:

```text
shared/LoRaEcdhRekey.h
test/test_lora_ecdh_rekey.cpp
```

The new header is included by `LoRaManager.h`, so it must be copied before the
gateway is compiled.

### 3. Default build

Do NOT enable ECDH yet.

The default is:

```text
FIELDRADIO_LORA_ECDH_REKEY_ENABLED=0
```

Therefore the existing HMAC/SHA-256 rotating-key path and wire protocol remain
unchanged.

### 4. Native scaffold test

The test validates:
- ECDH beacon envelope encoding/decoding;
- magic/version validation;
- 32-byte ephemeral/static public-key fields;
- two-epoch retention boundary.

Run:

```bash
pio test -e native
```

The standalone test was also compiled and executed with the default flag
(`FIELDRADIO_LORA_ECDH_REKEY_ENABLED=0`) during this audit.

### 5. Enabling the scaffold

Do not enable it in field firmware yet.

If you intentionally need to compile the future crypto path:

```text
-DFIELDRADIO_LORA_ECDH_REKEY_ENABLED=1
```

The source deliberately contains TODOs rather than guessing the exact
ESP-IDF/Mbed TLS X25519 API for the installed framework. The exact
`mbedtls_ecdh` API must be validated against the selected ESP-IDF/Mbed TLS
version before activation.

## TODO before activation

1. Load/create long-term X25519 keypair in protected NVS.
2. Generate one ephemeral keypair per 24-hour epoch.
3. Include ephemeral + static public keys in the authenticated beacon.
4. Authenticate/bind the long-term public key to the existing node identity.
5. Compute X25519 shared secret.
6. HKDF-SHA256 with domain separator + peer identity + epoch.
7. Maintain current and previous epoch keys for two epochs.
8. Explicitly erase expired ephemeral private keys/session keys.
9. Define future-clock handling and maximum acceptable clock skew.
10. Integrate session-key selection into packet V3 without changing legacy
    behavior when the feature flag is disabled.
11. Test two-node interoperability, reboot, deep-sleep, replay, clock skew,
    key rollover, packet loss, and rollback/fallback behavior.
12. Only then change the default flag to enabled in a future phase/release.

## Important

The current beacon implementation is intentionally unchanged. The new
`LoRaEcdhRekey::Beacon` structure specifies the future authenticated beacon
payload but does not transmit it yet. This prevents accidentally deploying a
partially implemented key exchange.
