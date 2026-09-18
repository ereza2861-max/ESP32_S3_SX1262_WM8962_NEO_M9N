# VERIFY5 — FieldRadio FASE 5 verification checklist

## A. Repository

- [ ] `git status` is clean before applying the change.
- [ ] `git apply --check PATCH5.diff` passes.
- [ ] `git diff --check` passes.
- [ ] `secrets/` is ignored.
- [ ] No private signing key is tracked.
- [ ] No MQTT password is tracked.
- [ ] No generated production firmware secret is tracked.
- [ ] No new broad `*.bin` ignore rule was introduced.

## B. Toolchain

- [ ] `pio --version` succeeds.
- [ ] `esptool version` or legacy equivalent succeeds.
- [ ] `espefuse version` or legacy equivalent succeeds.
- [ ] `espsecure version` or legacy equivalent succeeds.
- [ ] All Espressif tools come from a compatible toolchain.
- [ ] PlatformIO environment is exactly `esp32-s3-wroom-1`.

## C. Secure Boot key

- [ ] `secrets/secure_boot_signing_key.pem` exists only on the controlled
      manufacturing workstation/key-management system.
- [ ] File permission is `0600`.
- [ ] Existing key is never overwritten by provisioning.
- [ ] Key is RSA-3072 / Secure Boot V2.
- [ ] Public-key digest enrollment uses the intended `SECURE_BOOT_DIGEST0` slot.
- [ ] Digest block is checked as unused/correct before burn.

## D. Firmware signing

- [ ] Production build uses Release-mode Flash Encryption configuration.
- [ ] Build does not depend on a tracked private signing key.
- [ ] Bootloader is signed.
- [ ] Application is signed.
- [ ] Bootloader signature verification succeeds.
- [ ] Application signature verification succeeds.
- [ ] `espsecure signature-info-v2` is inspected when required.
- [ ] No unsigned production application is flashed.

## E. Flash Encryption

- [ ] A unique per-device Flash Encryption key is generated.
- [ ] Key is exactly 32 bytes for the selected `XTS_AES_128_KEY` profile.
- [ ] Key block is unused before burn.
- [ ] Key is burned with the correct key purpose.
- [ ] Key is not reused between devices.
- [ ] Production config selects `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y`.
- [ ] Development-mode Flash Encryption is not used.
- [ ] First boot is not interrupted during encryption.
- [ ] Host copy of the Flash Encryption key is removed after successful
      verification unless an explicit secure retention policy requires it.

## F. eFuse pre-check

Run:

```text
espefuse --port /dev/ttyUSB0 --chip esp32s3 summary
esptool --port /dev/ttyUSB0 flash-id
```

- [ ] Chip detection is ESP32-S3.
- [ ] Correct serial port is confirmed.
- [ ] Flash ID is recorded.
- [ ] Secure Boot digest slot state is recorded.
- [ ] Flash Encryption key block state is recorded.
- [ ] No unexpected eFuse security state is present.
- [ ] Manufacturing operator has read **EFUSE BURN IS IRREVERSIBLE**.

## G. eFuse burn

- [ ] Operator supplied `--burn`.
- [ ] Operator typed exactly `BURN-IRREVERSIBLE`.
- [ ] Secure Boot digest was enrolled.
- [ ] Flash Encryption key was enrolled.
- [ ] Secure Boot was explicitly enabled.
- [ ] `SPI_BOOT_CRYPT_CNT` was not prematurely burned against plaintext flash.
- [ ] Release-mode bootloader is responsible for the first encryption transition.
- [ ] No `--force` was used to bypass an eFuse safety check.

## H. Flashing and first boot

- [ ] Signed bootloader flashed at `0x0`.
- [ ] Partition table flashed at `0x8000`.
- [ ] Signed factory application flashed at `0x10000`.
- [ ] Device reset completed.
- [ ] First-boot flash encryption completed.
- [ ] Power was not interrupted during encryption.
- [ ] Device boots the production application.

## I. Security verification

- [ ] Secure Boot is reported enabled.
- [ ] Flash Encryption is reported enabled.
- [ ] Release-mode security state is recorded.
- [ ] UART download security policy is recorded.
- [ ] JTAG/security state is recorded.
- [ ] No unsigned/modified application is accepted in the controlled test.
- [ ] No production secret is printed into serial/build logs.

If Secure Download Mode has restricted `espefuse` access, do not defeat the
security setting merely to obtain another summary. Use the supported
security-information path and the saved pre-burn eFuse record.

## J. Firmware functionality

- [ ] HTTPS WebUI starts.
- [ ] WebUI authentication remains enabled.
- [ ] MQTT production security from FASE 3 remains enabled.
- [ ] MQTT does not fall back to plaintext port 1883 in production.
- [ ] BLE Sensor Reader starts.
- [ ] BLE Sensor node discovery/reconnect path remains available.
- [ ] LoRa P2P remains functional.
- [ ] LoRaWAN remains functional where provisioned.
- [ ] Existing audio/GNSS/storage functions are not regressed by the production
      security build.

## K. OTA

- [ ] OTA is **not** marked production-ready.
- [ ] Repository currently uses the no-OTA partition table.
- [ ] OTA signing is documented as FUTURE/TODO.
- [ ] No unsigned OTA workflow is advertised.

## L. Manufacturing record

Record only non-secret evidence:

- [ ] unit identifier / MAC or approved manufacturing identifier;
- [ ] hardware revision;
- [ ] PlatformIO/ESP-IDF/esptool toolchain version;
- [ ] firmware Git commit;
- [ ] firmware image hash;
- [ ] flash ID;
- [ ] pre-burn eFuse summary;
- [ ] post-boot security verification result;
- [ ] functional test result.

Never put private signing keys, Flash Encryption keys, MQTT passwords, or other
production credentials in the manufacturing record.
