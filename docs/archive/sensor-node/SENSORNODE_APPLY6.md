# APPLY5 — FieldRadio FASE 5

## 1. Apply the tracked-file patch

From the repository root:

```text
git apply PATCH5.diff
git diff --check
```

The patch changes only existing files. It does not add private keys, credentials,
or generated firmware artifacts.

## 2. Copy the new files

Copy these files from the FASE 5 delivery into the repository:

```text
docs/PRODUCTION.md
tools/provision.sh
```

Then:

```text
chmod +x tools/provision.sh
```

Do not copy any generated `secrets/` material into Git.

## 3. Tooling

The repository uses PlatformIO environment:

```text
esp32-s3-wroom-1
```

Install/activate a compatible PlatformIO + ESP-IDF/esptool toolchain. Verify:

```text
pio --version
esptool version
espefuse version
espsecure version
```

The script accepts either modern console names (`esptool`, `espefuse`,
`espsecure`) or legacy `.py` names.

## 4. Provisioning workflow

### Fresh production unit

```text
./tools/provision.sh --port /dev/ttyUSB0 --burn
```

The script:

1. inspects the target eFuse state;
2. creates `secrets/` with restrictive permissions;
3. creates the Secure Boot V2 RSA-3072 signing key if absent;
4. creates a unique per-device Flash Encryption key if absent;
5. prepares the production Release-mode build configuration;
6. builds the existing PlatformIO environment;
7. signs bootloader and application;
8. verifies both signatures;
9. asks for `BURN-IRREVERSIBLE`;
10. enrolls the Secure Boot digest;
11. enrolls the per-device Flash Encryption key;
12. enables Secure Boot;
13. flashes the signed first-boot images;
14. lets the Release-mode bootloader perform the first flash-encryption transition.

The script deliberately does **not** burn `SPI_BOOT_CRYPT_CNT` before flashing the
first-boot images. Doing so while the flash still contains plaintext would make
the bootloader interpret plaintext as ciphertext. The Release-mode bootloader
performs the first encryption transition using the pre-burned per-device key.

A fresh device therefore requires explicit `--burn`. A normal invocation does not
silently burn eFuses or perform an unsafe plaintext production flash.

### Build/sign/verify without hardware modification

```text
./tools/provision.sh --port /dev/ttyUSB0 --no-flash
```

This performs build, signing, and signature verification without eFuse writes or
flashing.

### Status only

```text
./tools/provision.sh --status --port /dev/ttyUSB0
```

This runs eFuse summary and `flash-id` without changing the device.

## 5. Development vs production

Do not use `make upload` as a substitute for production provisioning.

Development builds and production Release-mode builds have different security
assumptions. A production device must not be provisioned with a Development-mode
Flash Encryption configuration.

The FASE 4 ECDH flag remains unchanged and disabled.

## 6. eFuse safety

**EFUSE BURN IS IRREVERSIBLE.**

Before `--burn`, inspect the summary and verify:

- ESP32-S3 target;
- intended Secure Boot digest block;
- intended Flash Encryption key block;
- expected current security state;
- correct serial port.

The script requires the exact confirmation:

```text
BURN-IRREVERSIBLE
```

It does not accept `y` or `yes`.

## 7. Build/sign/flash sequence

The production artifact sequence is:

```text
build
  -> sign bootloader/app
  -> verify signatures
  -> burn Secure Boot digest + per-device Flash Encryption key
  -> enable Secure Boot
  -> flash signed first-boot images
  -> first boot encrypts flash in Release mode
```

The first boot must not be power-cycled while flash encryption is running.

## 8. Verification

After the device completes its first production boot, follow `VERIFY5.md` and
record the pre-burn and post-boot security evidence.

Because Release/Secure Download Mode can restrict later ROM-tool access, keep the
pre-burn eFuse summary as part of the manufacturing record and perform final
device-security verification through the supported security-information path.

OTA remains FUTURE/TODO in this repository.
