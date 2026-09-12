# FieldRadio — ESP32-S3-WROOM-1 firmware

Firmware baru ini ditujukan untuk PCB yang diberikan:
- ESP32-S3-WROOM-1
- SX1276
- WM8960
- NEO-M8N
- microSD

## Pin mapping PCB

`include/BoardConfig.h` and `docs/PCB_MAPPING.md` are the firmware pin-mapping source of truth. GPIO7 is the selected WM8960 ADCDAT input; the older GPIO34 reference is obsolete.

| Fungsi | GPIO |
|---|---:|
| SPI SCK | 12 |
| SPI MISO | 13 |
| SPI MOSI | 11 |
| SX1276 NSS | 10 |
| SX1276 RESET | 14 |
| SX1276 DIO0 | 2 |
| SX1276 DIO1 | 15 |
| microSD CS | 16 |
| GNSS RX | 18 |
| GNSS TX | 17 |
| WM8960 SDA | 8 |
| WM8960 SCL | 9 |
| WM8960 BCLK | 4 |
| WM8960 LRCLK | 5 |
| WM8960 DACDAT / ESP32-S3 TX | 6 |
| WM8960 ADCDAT / ESP32-S3 RX | 7 |
| WM8960 MCLK | external 24 MHz oscillator |

## Fitur

- GNSS position/altitude/satellite telemetry.
- LoRa text, SOS dan periodic position telemetry melalui SX1276.
- WAV recorder 44.1 kHz stereo 16-bit ke microSD.
- WAV playback 44.1 kHz stereo 16-bit.
- USB Audio Class (UAC) stereo speaker + microphone ke WM8960.
- Rekam WAV dengan source yang dapat dipilih: WM8960 MIC atau USB Audio; recording WM8960 MIC tidak lagi diblokir hanya karena USB Audio sedang aktif.
- Wi-Fi AP + HTTP dashboard.
- Basic-auth untuk endpoint web.
- Safe path validation untuk file deletion/playback.
- FreeRTOS task separation.
- Physical PTT/SOS, battery ADC, status LED, RX squelch and RX/TX audio feedback.
- Authenticated AES-128-CTR + HMAC-SHA256 LoRa payloads with voice sequence/CRC.
- SD track logging and recording-space rotation.
- Rate-limited web control and authenticated reboot transport.

## Catatan migrasi ESP32-S3 dan USB audio

ESP32-S3 tidak menyediakan Bluetooth Classic/A2DP, sehingga fitur A2DP dihapus. USB Audio memakai native USB D-/D+ pada GPIO19/GPIO20. Mapping GPIO di atas adalah mapping PCB baru untuk S3 dan memerlukan rerouting; firmware ini bukan drop-in replacement untuk PCB WROOM-32E lama. Espressif menyediakan `usb_device_uac` untuk ESP32-S3 dengan streaming speaker/mic, volume/mute dan feedback endpoint.

## Rekaman dan USB Audio

Source rekaman dapat dipilih dari dashboard web:
- `WM8960 MIC`: audio ADC WM8960 direkam ke WAV. Saat USB Audio aktif, data mic juga dicadangkan melalui ring buffer agar USB microphone tetap dapat mengirim audio ke host tanpa membaca I2S RX secara bersamaan.
- `USB Audio`: stream speaker USB dari host direkam ke WAV melalui ring buffer, sementara playback ke WM8960 tetap berjalan.

Source disimpan di NVS. Perubahan source ditolak saat recording atau playback sedang aktif. Buffer USB mencegah callback UAC melakukan operasi SD secara langsung.

## Catatan audio penting

PCB menggunakan oscillator WM8960 24 MHz dan codec dikonfigurasi sebagai I2S master.
ESP32 dikonfigurasi sebagai I2S slave. Ini berbeda dari firmware MicroMod lama.

## Catatan RF

`LORA_FREQ_MHZ` diset 923 MHz sebagai default untuk desain regional 920–923 MHz.
Verifikasi kembali terhadap izin frekuensi setempat dan matching network PCB sebelum TX.

## Catatan hardware

Package KiCad yang diberikan sendiri menyebut desain sebagai engineering seed/reference, bukan fabrication release.
ERC/DRC, simbol/footprint, power integrity, RF matching, antenna network, GNSS RF path,
dan level/clock audio tetap harus divalidasi pada PCB final.

## Flash / partition layout

This variant targets a 16 MiB flash device. The no-OTA partition table allocates the full remaining flash after the bootloader/table region to a single factory application (`0x10000..0xFFFFFF`). PlatformIO is explicitly configured for a 16 MiB flash image and matching maximum application size.

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

For a local deployment that needs Wi-Fi credentials, copy
`include/LocalConfig.example.h` to `include/LocalConfig.h` and edit the values.
`LocalConfig.h` is ignored by Git and is not created or injected by GitHub Actions.

The CI workflow deliberately does not pass credentials, tokens, or other secret
values to the compiler. It runs the repository preflight before compilation and
fails if a credential-like file or known secret pattern is present in tracked
content. The preflight suppresses matching content so a detected secret is not
printed into the Actions log.

## Audio capabilities and runtime controls

The audio subsystem supports WM8960 microphone, LINEIN2, LINEIN3 and USB
recording sources. The web UI exposes USB microphone monitoring, codec ADC/DAC
loopback, a short diagnostic tone generator, USB mute/volume state and a
peak/RMS/clipping level meter through `/api/status`.

USB playback remains USB Audio OUT -> I2S -> WM8960. USB recording/monitoring
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
dynamic MIC/SPK sampling-rate configuration. The WM8960 clock remains at the
PCB's fixed 44.1 kHz configuration. A future true dynamic-rate implementation
must replace or fork the UAC descriptor/driver rather than pretending that a
runtime packet-size check changes the negotiated USB format.
