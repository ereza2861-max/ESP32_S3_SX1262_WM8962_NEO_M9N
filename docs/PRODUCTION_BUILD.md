# Production build and hardware security

Secure Boot V2 and Flash Encryption are manufacturing operations. They are not
part of the normal `make build` path.

## Generate key material

Run:

```sh
make secure-boot-keys KEY_DIR=/secure/offline/fieldradio-keys
```

The target only creates:
- `secure_boot_signing_key.pem` — Secure Boot V2 signing key.
- `flash_encryption_key.hex` — 32-byte Flash Encryption key.

Keep this directory outside the Git worktree. The target does **not** burn
eFuses, modify the default PlatformIO environment, flash firmware, or place
private material in tracked files.

## Manufacturing sequence

1. Generate and escrow the keys on an offline manufacturing workstation.
2. Build the normal firmware from a reviewed commit.
3. Program the firmware and required provisioning material.
4. Burn the Secure Boot V2 and Flash Encryption eFuses using the Espressif
   manufacturing procedure appropriate to the exact ESP32-S3 module.
5. Enable the corresponding production boot configuration and sign the image.
6. Verify the device boots only the expected signed image and that encrypted
   flash contents are unreadable when inspected without the device key.
7. Record the device/eFuse provisioning result in the manufacturing system.

`TODO(hw):` exact eFuse commands and post-burn verification must be executed
against the final ESP32-S3-WROOM-1-N16R8 production fixture and must not
be embedded in a normal repository build target.
