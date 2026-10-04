# FieldRadio ESP32-S3 security provisioning

> **Project status (2026-09-17):** No node is operating and the PCB has not been fabricated. Provisioning procedures are development/design workflows and are not evidence of deployed production hardware. See `docs/PROJECT_STATUS.md`.

The firmware now reserves an NVS key partition and enables the ESP-IDF flash/NVS
encryption configuration in `sdkconfig.defaults`. This protects the existing
`Preferences` values (including Wi-Fi credentials and the LoRa key) only after
the device has completed the Espressif provisioning flow.

Do not treat `git apply` followed by a normal `pio run -t upload` as a complete
production security deployment. Flash encryption is persistent and can change
the recovery/update procedure.

## Secure Boot v2

Secure Boot v2 requires a real signing key and eFuse provisioning. It is
intentionally not forced by the default build because enabling it without the
correct signing/provisioning workflow can make a device unbootable.

For a production profile, enable:
- `CONFIG_SECURE_BOOT_V2_ENABLED=y`
- `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y`
- a controlled RSA signing key
- an appropriate secure UART ROM-download mode

Keep the private signing key out of the repository and CI logs.

## NVS encryption

The partition table contains `nvs_keys` at `0xE000`, immediately before
`phy_init`. ESP-IDF uses this key partition for encrypted NVS when the selected
flash-encryption key-protection scheme is active.

Existing NVS application code does not need to change API calls; `Preferences`
continues to use the encrypted NVS transparently.

## Key rotation

Automatic LoRa key rotation cannot safely be performed independently by a
node: peers would lose the ability to decrypt traffic. The firmware therefore
continues to accept a configured AES-128 key and protects it at rest through
NVS/flash encryption. A coordinated rekey protocol should distribute a new
key to all peers before activating it. Do not implement time-based unilateral
rotation.


## WebUI credential storage

The legacy `webpass` NVS key is migrated to a per-device salted iterative SHA-256
verifier (`websalt` + `webph`) on the first load/save. The cleartext password remains
in RAM only when it is supplied/configured at runtime because HTTP Basic authentication
requires the password for verification. This is **not** transport encryption.

For production, pair this with the Secure Boot/flash-encryption provisioning flow and
use a device-specific CA-issued certificate policy. The current WebUI transport is
HTTPS-only when provisioned; it deliberately does not fall back to plaintext HTTP.


## Secret storage hierarchy

The configuration/security decision uses a layered classification without
removing existing storage namespaces:

- non-secret configuration and operational state remain under the existing
  atomic A/B configuration manager;
- long-term device secrets use the encrypted-NVS/security-provisioning boundary
  already required by the applicable subsystem;
- ephemeral session keys/nonces that are intentionally RAM-only are not promoted
  to persistent configuration;
- `loraKeyHex` remains legacy application key material and is not a root key for
  encrypting unrelated secrets.

Production provisioning must use the secure production profile. Development/HIL
profiles may be used for validation, but they are not production-security
acceptance evidence.

## Sensor OTA session policy

Firmware uploads to the ESP32-C3 OTA WebUI require the provisioned OTA password
and the current `X-OTA-Session` value. The session is checked when the upload
starts and again when the upload completion handler accepts the resulting image.
The existing profile endpoint retains `X-Profile-Cables-Changed: true` in
addition to its password/session authentication.

This policy is an additive authorization check; it does not change the existing
OTA AP transport or introduce a new library.

## ECDH production gate
Production builds compile with ECDH rekey policy forced to zero. Development/HIL may exercise ECDH. Production enablement additionally requires the existing Secure Boot/Flash Encryption evidence gate.
