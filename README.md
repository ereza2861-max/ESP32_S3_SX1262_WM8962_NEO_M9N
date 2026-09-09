# FieldRadio — ESP32-WROOM-32E PCB firmware

Firmware baru ini ditujukan untuk PCB yang diberikan:
- ESP32-WROOM-32E
- SX1276
- WM8960
- NEO-M8N
- microSD

## Pin mapping PCB

| Fungsi | GPIO |
|---|---:|
| SPI SCK | 18 |
| SPI MISO | 19 |
| SPI MOSI | 23 |
| SX1276 NSS | 27 |
| SX1276 RESET | 26 |
| SX1276 DIO0 | 35 |
| SX1276 DIO1 | 36 |
| microSD CS | 13 |
| GNSS RX | 16 |
| GNSS TX | 17 |
| WM8960 SDA | 21 |
| WM8960 SCL | 22 |
| WM8960 BCLK | 32 |
| WM8960 LRCLK | 33 |
| WM8960 DACDAT / ESP32 TX | 25 |
| WM8960 ADCDAT / ESP32 RX | 34 |
| WM8960 MCLK | external 24 MHz oscillator |

## Fitur

- GNSS position/altitude/satellite telemetry.
- LoRa text, SOS dan periodic position telemetry melalui SX1276.
- WAV recorder 44.1 kHz stereo 16-bit ke microSD.
- WAV playback 44.1 kHz stereo 16-bit.
- Bluetooth A2DP sink ke WM8960.
- Wi-Fi AP + HTTP dashboard.
- Basic-auth untuk endpoint web.
- Safe path validation untuk file deletion/playback.
- FreeRTOS task separation.
- OTA endpoint tetap disabled sampai implementasi signed OTA ditambahkan.

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

## Build

```text
pio run
pio run -t upload
pio device monitor
```
