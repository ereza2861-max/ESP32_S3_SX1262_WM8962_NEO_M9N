# Local device provisioning

This repository deliberately does not contain device credentials, TLS private keys,
TLS certificates, or generated TLS headers. A fresh clone therefore needs one local
provisioning step before a production/local firmware build.

## Fresh clone to HTTPS WebUI

```text
git clone <repository>
cd <repository>
make provision
make check-provisioning
make build
make upload
make monitor
```

`make provision` creates:

```text
include/LocalConfig.h
secrets/web_tls_cert.der
secrets/web_tls_key.der
```

All three are ignored by Git. The PlatformIO pre-build script then generates the
ignored `include/generated/WebTlsProvisioning.h` from the DER files.

The provisioning command prompts for the AP password, WebUI password, and LoRa
AES-128 key. If the LoRa key is left blank, a random 128-bit key is generated.
AP and WebUI passwords must be different and both must be at least 8 characters.

The generated lab certificate defaults to:

- DNS SAN: `fieldradio.local`
- IP SAN: `192.168.4.1`
- validity: 825 days

These defaults match the current ESP32 SoftAP address/name assumptions. If the
deployment changes the address or DNS name, regenerate the certificate with:

```text
FIELDRADIO_TLS_IP=<actual-ip> FIELDRADIO_TLS_DNS=<actual-name> FORCE=1 make provision
```

The certificate is self-signed for lab use. A browser will normally show a trust
warning until the certificate/CA is explicitly trusted. For production, replace
the generated DER pair with a device-specific CA-issued certificate and matching
private key, then run `make check-provisioning`. Do not put either file in Git.

## What is tracked versus local

| Material | Tracked | Purpose |
|---|---:|---|
| `include/LocalConfig.example.h` | yes | safe template |
| `tools/provision-device.sh` | yes | creates local credentials/TLS |
| `tools/check-provisioning.sh` | yes | validates provisioning |
| `tools/provision-web-tls.py` | yes | embeds DER into generated header |
| `include/LocalConfig.h` | no | device credentials |
| `secrets/web_tls_cert.der` | no | device TLS certificate |
| `secrets/web_tls_key.der` | no | device TLS private key |
| `include/generated/WebTlsProvisioning.h` | no | generated C++ TLS material |

`make build` and `make upload` now require local provisioning. The CI-only
`make ci-build` target remains allowed to build without secrets; in that mode the
WebUI is compiled with HTTPS disabled rather than falling back to HTTP.

## TLS validation

`make check-provisioning` verifies:

1. the certificate parses as DER X.509;
2. the private key parses as DER key material;
3. the certificate and private key contain the same public key;
4. local credential lengths and the LoRa key format satisfy the firmware contract.

The PlatformIO pre-build hook repeats certificate/key validation and refuses a
partial or mismatched pair. Missing TLS material is tolerated only for the
CI-safe build path.

## Flashing

PlatformIO normally auto-detects the serial port:

```text
make upload
```

If multiple boards are connected:

```text
make upload UPLOAD_PORT=/dev/ttyUSB0
```

On macOS the port may be `/dev/cu.usbmodem...`; on Windows use the corresponding
COM port.

This workflow only provisions application-level credentials and WebUI TLS. It
does not provision Secure Boot v2, flash encryption eFuses, or a production
signing key. Follow `docs/SECURITY_PROVISIONING.md` separately for those
irreversible production security steps.
