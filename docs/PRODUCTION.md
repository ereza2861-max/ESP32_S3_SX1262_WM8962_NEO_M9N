# FieldRadio production manufacturing

> **Scope: FASE 5 only.** This document covers manufacturing documentation and
> production provisioning for the ESP32-S3-WROOM-1-N16R8 target. It does not
> enable the FASE 4 X25519/ECDH scaffold and does not change the existing LoRa
> encryption protocol.

## 1. Production boundary

A production clone must keep all device secrets outside Git:

- Secure Boot V2 signing private key: `secrets/secure_boot_signing_key.pem`
- Per-device Flash Encryption key: `secrets/flash_encryption_key.bin` **only until
  the initial ciphertext images have been flashed and verified**
- Device application credentials and TLS private material remain under the existing
  `secrets/` / ignored local provisioning boundary.

`*.pem`, `*.key`, `*.der`, `*.crt` and `secrets/` are already ignored by this
repository. Do not broaden `.gitignore` to ignore every `*.bin`; firmware
artifacts may be useful to the repository/CI. Keep the per-device encryption key
inside `secrets/`.

**EFUSE BURN IS IRREVERSIBLE.**

A wrong key, key-purpose, security-download setting, or other eFuse selection can
make a device permanently unusable. Never use `--force` merely to overcome an
eFuse refusal. Stop and inspect the current state first.

## 2. Prerequisites

Use one consistent ESP-IDF/esptool toolchain for the whole manufacturing run.

Required:

- PlatformIO CLI (`pio`)
- Python 3
- Espressif `esptool`, `espefuse`, and `espsecure`
- A working PlatformIO ESP32-S3 environment from `platformio.ini`
- USB/serial access to the target
- A genuine ESP32-S3 target matching `ESP32_S3_WROOM_1_N16R8`
- A production hardware revision approved for manufacturing

The repository currently defines the PlatformIO environment:

```text
esp32-s3-wroom-1
```

Do not substitute another PlatformIO environment without first auditing
`platformio.ini`.

For modern esptool v5 the console names are `esptool`, `espefuse`, and
`espsecure`; older v4 installations may expose the `.py` names. The provisioning
script accepts either spelling.

Before a manufacturing run:

```text
python3 --version
pio --version
esptool version
espefuse version
espsecure version
```

All three Espressif tools must belong to the same compatible toolchain. Do not mix
an old `espefuse.py` with a different-generation `espsecure`.

## 3. Secure Boot V2

ESP32-S3 uses Secure Boot V2 with RSA-3072/RSA-PSS for the RSA scheme. The ROM
bootloader loads the second-stage bootloader and the second-stage bootloader
verifies signed images. The public-key digest is stored in eFuse; the private
signing key remains off-device.

Generate the signing key once:

```text
espsecure generate-signing-key --version 2 --scheme rsa3072 \
  secrets/secure_boot_signing_key.pem
chmod 600 secrets/secure_boot_signing_key.pem
```

The repository provisioning script generates this key if it is absent and never
overwrites an existing key.

For the current repository, the production build is intentionally prepared for
**external signing**. The signing key is not injected into a tracked file or CI
configuration. The script signs the bootloader and application after the build.

Verify an image before flashing:

```text
espsecure verify-signature --version 2 \
  --keyfile secrets/secure_boot_signing_key.pem \
  <signed-image.bin>
```

For an external/manual-signing workflow, the bootloader must also be signed. The
bootloader is not an OTA-updatable artifact; it is a factory artifact.

After signing, inspect the signature information when required:

```text
espsecure signature-info-v2 <signed-image.bin>
```

Secure Boot V2 does not make an unsigned OTA image acceptable. Once hardware
Secure Boot is active, an application image must carry a valid signature whose
public-key digest matches an enrolled eFuse digest.

## 4. Flash Encryption — production/release mode

Use **Release mode**, not Development mode.

Development mode leaves more serial-download capabilities available and is intended
for development. Release mode is the manufacturing/production configuration and
prevents the UART bootloader from performing the development-style plaintext
flash-encryption workflow.

The repository's secure defaults therefore select:

```text
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
```

The production build also uses Secure Boot V2 and secure UART download settings.
Do not copy a development `sdkconfig` over the production configuration.

### Per-device encryption key

If the manufacturing process uses a host-generated XTS-AES key, the key must be
unique per device. Never reuse one flash-encryption key across production units.

For the script's default AES-128 XTS profile, generate a 256-bit raw key:

```text
python3 - <<'PY'
from pathlib import Path
import secrets
Path("secrets/flash_encryption_key.bin").write_bytes(secrets.token_bytes(32))
PY
chmod 600 secrets/flash_encryption_key.bin
```

The key is burned to an unused ESP32-S3 key block with purpose
`XTS_AES_128_KEY`. The exact block is selected by the manufacturing script and
must be verified against the pre-burn eFuse summary.

After the device has been provisioned, ciphertext images are produced from the
**signed** images. Signing must happen before flash encryption because the
signature covers the image contents.

Example:

```text
espsecure encrypt-flash-data --aes-xts \
  --keyfile secrets/flash_encryption_key.bin \
  --address 0x10000 \
  --output artifacts/firmware.enc.bin \
  artifacts/firmware.signed.bin
```

The actual offsets must match the repository partition table. For this repository,
the factory application starts at `0x10000`; the script refuses to silently invent
different offsets.

The encrypted device decrypts the firmware transparently at runtime. Flash
encryption also covers the second-stage bootloader, partition table, NVS key
partition and application partitions according to ESP-IDF's flash-encryption
rules.

**Do not interrupt power during a first-boot in-place encryption operation.**
This repository's production workflow prefers host-encrypted ciphertext flashing
so the release-mode device is not intentionally exposed to a plaintext serial
flash operation.

### Host key disposal

If a host-generated flash-encryption key is used, delete the local copy after the
initial ciphertext images have been flashed and verified, unless the organization's
documented production recovery policy explicitly requires retaining it in a
dedicated secure key-management system. Never leave the key in the repository,
CI artifacts, build output, or a normal workstation backup.

## 5. eFuse procedure

Always inspect first:

```text
espefuse --port /dev/ttyUSB0 --chip esp32s3 summary
```

Also record:

```text
esptool --port /dev/ttyUSB0 flash-id
```

The summary must be reviewed before any burn. Confirm:

- detected chip is ESP32-S3;
- Secure Boot digest slot selected for this device is unused or matches the
  intended signing public key;
- Flash Encryption key block selected for this device is unused;
- key purposes are correct;
- Secure Boot is not already enabled with an unknown digest;
- Flash Encryption is not already enabled with an unknown key;
- no required eFuse write-protection state prevents the planned operation;
- the serial port belongs to the intended unit.

The production script uses `--burn` plus the exact confirmation token
`BURN-IRREVERSIBLE`. It never accepts `y`, `yes`, or an empty confirmation.

The script does not use `--force` for eFuse writes.

For ESP32-S3 Secure Boot V2, the public key digest can be enrolled with the
`espefuse burn-key-digest` command, for example:

```text
espefuse --port /dev/ttyUSB0 --chip esp32s3 burn-key-digest \
  BLOCK_KEY0 secrets/secure_boot_signing_key.pem SECURE_BOOT_DIGEST0
```

The exact key block must be free and verified before use.

For a pre-generated AES-128 XTS flash-encryption key:

```text
espefuse --port /dev/ttyUSB0 --chip esp32s3 burn-key \
  BLOCK_KEY1 secrets/flash_encryption_key.bin XTS_AES_128_KEY
```

The exact key block must be free and verified before use.

Secure Boot V2 and Flash Encryption are not the only security eFuses. Release
mode also changes download/JTAG-related security state. These changes must be
performed only after the complete key material has been enrolled and the firmware
artifact has been validated.

**EFUSE BURN IS IRREVERSIBLE.**

After every burn stage, run another summary and compare the state with the
manufacturing record.

## 6. Verification

Before burn:

```text
espefuse --port /dev/ttyUSB0 --chip esp32s3 summary
esptool --port /dev/ttyUSB0 flash-id
```

After key enrollment and security-state programming:

```text
espefuse --port /dev/ttyUSB0 --chip esp32s3 summary
```

Confirm the intended Secure Boot digest and Flash Encryption key purpose are
present, Secure Boot is enabled, and Flash Encryption is enabled.

Do not expect the secret Flash Encryption key bytes to be readable after the
normal protection is applied.

The final device checks should include:

- signed production firmware boots;
- Secure Boot rejects a deliberately modified application in a controlled
  manufacturing test;
- Flash Encryption is reported enabled in the intended release configuration;
- `flash-id` reports the expected flash device;
- HTTPS WebUI starts with the already-provisioned TLS material;
- MQTT production security from FASE 3 remains intact;
- BLE Sensor Reader is present;
- existing LoRa P2P/LoRaWAN functionality is not regressed.

## 7. OTA signing

OTA signing is a **FUTURE/TODO** for this repository because the current partition
table is explicitly no-OTA (`no_ota.csv`).

Do not claim OTA production support merely because Secure Boot is enabled.

When OTA is implemented, every OTA application image must be signed with an
authorized Secure Boot key and the bootloader must verify the signature before
accepting the image. Flash encryption then encrypts the written application
partition.

A future OTA design must also define key rotation/revocation, rollback behavior,
anti-rollback policy if required, and recovery from a failed update.

## 8. Recovery and limits

After Secure Boot and Release-mode Flash Encryption are active:

- unsigned or modified firmware must not be accepted;
- plaintext serial reflashing is not a normal recovery path;
- a normal erase/flash development workflow must not be used;
- bootloader changes require the Secure Boot signing/reflash policy to be
  satisfied;
- OTA is the intended future update mechanism for field firmware;
- permanent eFuse mistakes cannot be undone.

Secure Download Mode, if selected, restricts ROM download operations. Permanently
disabling ROM Download Mode is an even stronger production choice, but once
disabled it prevents normal `esptool`/`espefuse` access. Choose the policy before
final eFuse locking.

## 9. Manufacturing checklist

### Before burn

- [ ] Repository is clean.
- [ ] `secrets/` is outside Git tracking.
- [ ] Secure Boot private key is present and permission `0600`.
- [ ] Per-device Flash Encryption key is unique.
- [ ] PlatformIO environment is `esp32-s3-wroom-1`.
- [ ] Production secure configuration selects Release mode.
- [ ] Bootloader and application have been signed.
- [ ] Both signatures verify.
- [ ] Target chip is ESP32-S3.
- [ ] `flash-id` matches the expected hardware.
- [ ] eFuse summary is recorded.
- [ ] Selected Secure Boot digest block is free/correct.
- [ ] Selected Flash Encryption key block is free/correct.
- [ ] Operator understands **EFUSE BURN IS IRREVERSIBLE.**

### After burn

- [ ] eFuse summary recorded again.
- [ ] Secure Boot state verified.
- [ ] Flash Encryption state verified.
- [ ] Release-mode/download security state verified.
- [ ] Ciphertext images, not plaintext production images, were flashed when
      Release-mode host encryption is used.
- [ ] Device boots successfully.
- [ ] HTTPS WebUI verified.
- [ ] MQTT production security verified.
- [ ] BLE Sensor Reader verified.
- [ ] LoRa P2P/LoRaWAN verified.

### Before field shipment

- [ ] No private signing key is in the repository or firmware artifact.
- [ ] No MQTT password is in Git or build logs.
- [ ] Per-device flash-encryption key has been removed from the normal workstation
      if the recovery policy does not require retention.
- [ ] Manufacturing record contains chip identity and eFuse verification result,
      not secret key material.
- [ ] OTA remains explicitly marked FUTURE/TODO until implemented and tested.
