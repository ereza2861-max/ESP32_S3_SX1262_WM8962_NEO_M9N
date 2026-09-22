# FieldRadio — ESP32-S3-WROOM-1-N16R8 firmware

> **Status proyek (2026-09-17):** Belum ada node yang beroperasi dan PCB belum diproduksi. Rev-C adalah target routing/design revision. Rolling/perubahan pin masih diterima selama belum ada keterangan eksplisit bahwa PCB sudah diproduksi. Lihat `docs/PROJECT_STATUS.md` sebagai source of truth status proyek.

Repository: `ESP32_S3_SX1262_WM8962_NEO_M9N`

Firmware baru ini ditujukan untuk **target PCB/routing Rev-C**:
- ESP32-S3-WROOM-1-N16R8 (16 MB Quad SPI flash + 8 MB Octal SPI PSRAM)
- SX1262
- WM8962
- NEO-M9N
- microSD

## Pin mapping PCB

`include/BoardConfig.h` and `docs/PCB_MAPPING.md` are the firmware pin-mapping source of truth; the physical PCB is not yet fabricated. GPIO7 is the selected WM8962 ADCDAT input; the older GPIO34 reference is obsolete.

| Fungsi | GPIO |
|---|---:|
| SPI SCK | 12 |
| SPI MISO | 13 |
| SPI MOSI | 11 |
| SX1262 NSS | 10 |
| SX1262 RESET | 17 |
| SX1262 DIO1 / IRQ | 14 |
| SX1262 BUSY | 15 |
| microSD CS | 16 |
| GNSS RX | 44 |
| GNSS TX | 43 |
| WM8962 SDA | 38 |
| WM8962 SCL | 48 |
| WM8962 BCLK | 4 |
| WM8962 LRCLK | 5 |
| WM8962 DACDAT / ESP32-S3 TX | 6 |
| WM8962 ADCDAT / ESP32-S3 RX | 7 |
| Battery ADC | 1 |
| MAX2016 forward ADC | 2 |
| MAX2016 reflected ADC | 8 |
| GNSS 1-PPS | 9 |
| PTT | 21 |
| SOS | 18 |
| TX indicator LED | 42 |
| Addressable RGB | 39 |
| Haptic | 40 |
| Charging indicator | 41 |
| Buzzer | 47 |
| Native USB D-/D+ | 19 / 20 |
| WM8962 MCLK | external 24 MHz oscillator |


## LoRaWAN Class A

FieldRadio can optionally operate the existing SX1262 as a **LoRaWAN Class A
end-device** using RadioLib's open-source LoRaWAN implementation. It supports
OTAA and ABP and exposes AS923-1/2/3/4 profiles; the default is AS923-2 for
Indonesia. LoRaWAN is opt-in and shares the physical SX1262 with the existing
P2P protocol through `RadioArbiter`.

Configure credentials from the authenticated HTTPS WebUI under the **LoRaWAN**
card or use the serial commands `lw status`, `lw connect`, `lw disconnect` and
`lw uplink <hex>`. Credentials are stored in NVS and production provisioning
must enable NVS encryption + flash encryption.

The existing Wi-Fi AP is intentionally retained because it allows direct
phone/tablet access when the device is deployed without a router. The
optional STA/MQTT path is intentionally not the BLE Sensor Reader transport; STA-only provisioning would create
a router dependency.

See `docs/LORAWAN.md` for provisioning, regional notes, payload format and
known limitations.

## Fitur

- GNSS position/altitude/satellite telemetry.
- LoRa text, SOS dan periodic position telemetry melalui SX1262.
- WAV recorder 44.1 kHz stereo 16-bit ke microSD.
- WAV playback 44.1 kHz stereo 16-bit.
- USB Audio Class (UAC) stereo speaker + microphone ke WM8962.
- Rekam WAV dengan source yang dapat dipilih: WM8962 MIC atau USB Audio; recording WM8962 MIC tidak lagi diblokir hanya karena USB Audio sedang aktif.
- Wi-Fi AP + HTTPS dashboard (port 443; requires provisioned TLS material).
- Basic-auth untuk endpoint web.
- Safe path validation untuk file deletion/playback.
- FreeRTOS task separation.
- Physical PTT/SOS, battery ADC, addressable RGB status, TX indicator, RX status through RGB, RX squelch and RX/TX audio feedback.
- Authenticated AES-128-CTR + HMAC-SHA256 LoRa payloads with voice sequence/CRC.
- SD track logging and recording-space rotation.
- Rate-limited web control and authenticated reboot transport.

## Catatan migrasi ESP32-S3 dan USB audio

ESP32-S3 tidak menyediakan Bluetooth Classic/A2DP, sehingga fitur A2DP dihapus. USB Audio memakai native USB D-/D+ pada GPIO19/GPIO20. Mapping GPIO di atas adalah candidate mapping PCB baru untuk S3 dan memerlukan rerouting sebelum fabrication; firmware ini bukan drop-in replacement untuk PCB WROOM-32E lama. Espressif menyediakan `usb_device_uac` untuk ESP32-S3 dengan streaming speaker/mic, volume/mute dan feedback endpoint.

## Rekaman dan USB Audio

Source rekaman dapat dipilih dari dashboard web:
- `WM8962 MIC`: audio ADC WM8962 direkam ke WAV. Saat USB Audio aktif, data mic juga dicadangkan melalui ring buffer agar USB microphone tetap dapat mengirim audio ke host tanpa membaca I2S RX secara bersamaan.
- `USB Audio`: stream speaker USB dari host direkam ke WAV melalui ring buffer, sementara playback ke WM8962 tetap berjalan.

Source disimpan di NVS. Perubahan source ditolak saat recording atau playback sedang aktif. Buffer USB mencegah callback UAC melakukan operasi SD secara langsung.

## Catatan audio penting

PCB menggunakan oscillator WM8962 24 MHz dan codec dikonfigurasi sebagai I2S master.
ESP32 dikonfigurasi sebagai I2S slave. Ini berbeda dari firmware MicroMod lama.

## Catatan RF

`LORA_FREQ_MHZ` diset 923 MHz sebagai default untuk desain regional 920–923 MHz.
Verifikasi kembali terhadap izin frekuensi setempat dan matching network PCB sebelum TX.

## Catatan hardware

Package KiCad yang diberikan sendiri menyebut desain sebagai engineering seed/reference, bukan fabrication release.
ERC/DRC, simbol/footprint, power integrity, RF matching, antenna network, GNSS RF path,
dan level/clock audio tetap harus divalidasi pada PCB final.

## Flash / partition layout

This variant targets the ESP32-S3-WROOM-1-N16R8 module (16 MiB Quad SPI flash + 8 MiB Octal SPI PSRAM). The no-OTA partition table allocates the full remaining flash after the bootloader/table region to a single factory application (`0x10000..0xFFFFFF`). PlatformIO is explicitly configured for QIO flash + OPI PSRAM, a 16 MiB flash image, and the matching maximum application size. The build is compile-time pinned to this module variant.

## BLE Sensor Node processor

The repository contains a separate **ESP32-C3 DevKitM-1** BLE sensor-node
firmware under `sensor_node_esp32c3/`. It is not the gateway processor and must
not be provisioned with the ESP32-S3 production script.

The canonical sensor-node documentation is `docs/SENSOR_NODE.md`. The root CI
builds both processors so a gateway-only green build cannot hide a broken
sensor-node firmware.

Repository layout:

```text
.
├── include/                  ESP32-S3 gateway headers
├── src/                      ESP32-S3 gateway firmware
├── shared/                   Gateway/sensor-node wire contract
├── sensor_node_esp32c3/      ESP32-C3 BLE sensor-node PlatformIO project
├── test/                     Native and HIL tests
├── tools/                    Provisioning and repository checks
├── docs/                     Canonical and historical project documentation
├── platformio.ini            ESP32-S3 gateway build
└── Makefile                  Common build/provisioning entry points
```

## Build

PlatformIO is the single build backend for both local development and GitHub Actions.
The repository `Makefile` provides the same entry points locally and in CI.

```text
make build
make clean
make upload
make monitor
make preflight
```

Cloud compiling is available through GitHub Actions; no local PlatformIO setup is
required for the cloud build. The workflow uploads successful firmware artifacts
and always uploads the compile log. See `docs/GITHUB_ACTIONS_BUILD.md` for web
and optional `gh` download workflows.

For a fresh local deployment, run the complete provisioning flow before a
normal build:

```text
make provision
make check-provisioning
make build
make upload
make monitor
```

`make provision` creates the ignored `include/LocalConfig.h` plus the ignored
device-specific TLS files `secrets/web_tls_cert.der` and
`secrets/web_tls_key.der`. The PlatformIO pre-build hook converts the DER pair
into the ignored `include/generated/WebTlsProvisioning.h`. No private key or
certificate is committed to Git. See `docs/LOCAL_PROVISIONING.md` for the
tracked/local material boundary and production certificate replacement.

The normal `make build` and `make upload` targets refuse to run without a
complete local provisioning set. GitHub Actions uses `make ci-build`, which
deliberately permits a secret-free compile and keeps HTTPS disabled in that
CI artifact rather than silently falling back to HTTP.

The CI workflow deliberately does not pass credentials, tokens, or other secret
values to the compiler. It runs the repository preflight before compilation and
fails if a credential-like file or known secret pattern is present in tracked
content. The preflight suppresses matching content so a detected secret is not
printed into the Actions log.

## Audio capabilities and runtime controls

The audio subsystem supports WM8962 microphone, LINEIN2, LINEIN3 and USB
recording sources. The web UI exposes USB microphone monitoring, codec ADC/DAC
loopback, a short diagnostic tone generator, USB mute/volume state and a
peak/RMS/clipping level meter through `/api/status`.

USB playback remains USB Audio OUT -> I2S -> WM8962. USB recording/monitoring
uses the UAC input callback. Codec-side ALC and noise gate are enabled only for
the microphone source; line inputs are kept at unity boost to avoid applying
microphone dynamics processing to external line-level equipment.

The firmware now enables ESP-SR AEC for the LoRa voice capture path when a
USB speaker reference is available. AEC runs at 16 kHz and uses the USB speaker
stream as the far-end reference; if no reference is available, voice capture
falls back to the raw microphone path.

USB packet-rate detection is also exposed as `usbSampleRate` in `/api/status`.
This is diagnostic/adaptive transport support, not true host-side sample-rate
negotiation: `espressif/usb_device_uac` 1.3.1 explicitly does not support
dynamic MIC/SPK sampling-rate configuration. The WM8962 clock remains at the
PCB's fixed 44.1 kHz configuration. A future true dynamic-rate implementation
must replace or fork the UAC descriptor/driver rather than pretending that a
runtime packet-size check changes the negotiated USB format.


### GNSS PPS and battery gauge

GNSS 1-PPS is routed to GPIO9 and is no longer reserved for a future analog device. The
firmware captures PPS edges and periodically synchronizes system UTC from validated GNSS time.
Deep sleep uses a 12-hour timer wake to force a periodic time-sync opportunity. The MAX17048
battery gauge at I2C address `0x36` is polled periodically with bounded bus operations and
falls back to the ADC1 battery path if unavailable. See `docs/PCB_MAPPING.md` for the hardware
contract.

## BLE pairing (per-node passkey)
Gateway BLE pairing uses a six-digit passkey stored per sensor identity in the `ble_peer` NVS namespace. The gateway refuses a secure connection when no passkey is provisioned; there is no insecure fallback. Use the authenticated WebUI endpoints `/api/ble/passkey` or the serial commands `ble passkey <addr> <passkey>`, `ble forget <addr>`, and `ble list`. Passkeys are masked in listings.

## BLE identity (RPA handling)
ESP32-C3 sensor nodes request a stable public BLE address. The gateway registry uses that identity address as its primary key and may retain a last-seen resolvable random address (RPA) for diagnostics. Unknown RPAs are not inserted as new nodes. An RPA older than one hour without a successful pairing is ignored by the peer mapping.

## Sensor queue policy
Sensor forwarding uses a fixed depth-16 queue. The default `DROP_OLDEST` policy keeps the newest sample when saturated and increments `sensorDropped`. `/api/sensors/live` exposes `sensorDropped` and `queueDepth`; runtime policy can be changed with `/api/sensors/queue-policy`.

## Version pinning
`platformio.ini` pins the active pioarduino 55.03.39 platform (Arduino-ESP32 3.3.9 / ESP-IDF 5.5.4). Dependencies whose exact upstream version could not be established from the repository are explicitly marked in `VERSIONS.md`; `tools/audit_versions.py` reports remaining pinning work. Run `python tools/audit_versions.py` in CI and before release.

## PKI Certificate Lifecycle

D-06 now uses a vendor-neutral RFC 7030 EST certificate lifecycle. The firmware
uses the factory-provisioned device certificate for EST mTLS bootstrap/renewal,
generates a fresh P-256 key pair for renewal, submits a PKCS#10 CSR, validates
the issued X.509 certificate, and commits the new certificate/key through an
NVS A/B commit-marker scheme. Automatic lifecycle is disabled by default.

The configured EST URL is an RFC 7030 endpoint (or backend adapter), while the
MQTT broker remains the certificate-validation authority for MQTT mTLS. See
`docs/MQTT_PKI_LIFECYCLE.md`.
