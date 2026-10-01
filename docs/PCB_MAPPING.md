# PCB mapping source of truth — ESP32-S3-WROOM-1-N16R8

> **Status proyek (2026-09-17):** Belum ada node yang beroperasi dan PCB belum diproduksi. Rev-C di sini adalah target/candidate routing contract. Rolling/perubahan pin masih diterima selama belum ada keterangan eksplisit bahwa PCB sudah diproduksi. Lihat `docs/PROJECT_STATUS.md`.

Mapping berikut adalah **source of truth Rev-C** untuk routing firmware ESP32-S3-WROOM-1-N16R8.
Nilai pin harus identik dengan `include/BoardConfig.h`; jangan membuat alias pin
alternatif di schematic/KiCad tanpa mengubah kedua dokumen dan firmware secara atomik.
Semua net pada tabel di bawah adalah candidate routing Rev-C dan menjadi basis rerouting dari mapping ESP32-WROOM-32E lama; ini belum merupakan bukti routing PCB fisik telah diproduksi.

Behavioral contract perangkat untuk RGB, TX LED, haptic, buzzer, charging-estimate LED,
dan battery fallback dipelihara di `docs/BEHAVIORAL_CONTRACT.md`.

## Normalized GPIO policy

- **ADC1 (GPIO1..10)** adalah domain analog utama. Semua input analog onboard tetap di
  ADC1 agar pembacaan tidak bergantung pada konflik ADC2/Wi-Fi.
- **GPIO9 dipakai sebagai GNSS 1-PPS input**, sehingga GPIO9 tidak lagi dicadangkan
  untuk perangkat analog masa depan. PPS adalah input digital/timing dan tidak dipakai
  sebagai ADC oleh firmware.
- GPIO4..7 tetap dipakai I2S digital; GPIO8 dipakai MAX2016 reflected ADC; GPIO10 dipakai
  SX1262 NSS. Jadi jangan menganggap seluruh rentang GPIO3..10 sebagai spare analog.
- **ADC2 (GPIO11..20) aman sebagai GPIO digital saat Wi-Fi aktif**. Konfliknya berlaku
  pada akses ADC2 sebagai *analog ADC*, bukan pada `digitalRead/digitalWrite` atau peripheral
  digital seperti SPI. Karena itu GPIO11/12/13 untuk SPI dan GPIO14..18 untuk kontrol radio/
  tombol tetap valid. GPIO19/20 tetap dicadangkan untuk native USB dan bukan spare ADC2.
- GPIO0/3/45/46 adalah **strapping pins**; jangan dipakai untuk LED atau output yang dapat
  mengubah level strap saat reset. GPIO26..37 tidak dipakai karena paket WROOM-1-N16R8
  menggunakan jalur internal Octal Flash/PSRAM pada kelompok tersebut.
- GPIO39..42 membawa fungsi JTAG, dan GPIO43/44 adalah UART0 default. Keduanya tetap dapat
  dipakai melalui GPIO Matrix setelah boot, tetapi bukan pilihan untuk fungsi boot-critical.
- GPIO48 sekarang dipakai sebagai I2C SCL. Dedicated RX LED dihapus sebagai net terpisah
  karena status RX sudah ditampilkan oleh addressable RGB LED.

## Pin map

| Fungsi | GPIO | Domain / catatan |
|---|---:|---|
| Battery ADC | 1 | ADC1; future-ADC policy |
| MAX2016 forward | 2 | ADC1 |
| GPIO3 | 3 | **Spare hanya secara logika, tetapi strapping; jangan dirouting sebagai sensor/LED** |
| WM8962 BCLK | 4 | ADC1-capable, dipakai digital I2S |
| WM8962 LRCLK | 5 | ADC1-capable, dipakai digital I2S |
| WM8962 DACDAT | 6 | ADC1-capable, dipakai digital I2S |
| WM8962 ADCDAT | 7 | ADC1-capable, dipakai digital I2S |
| MAX2016 reflected | 8 | ADC1 |
| **GNSS 1-PPS** | **9** | **digital timing input; ADC1-capable but not used as ADC** |
| SX1262 NSS | 10 | ADC1-capable, dipakai digital CS |
| Shared SPI MOSI | 11 | ADC2; digital-only in this design |
| Shared SPI SCK | 12 | ADC2; digital-only in this design |
| Shared SPI MISO | 13 | ADC2; digital-only in this design |
| SX1262 DIO1 / IRQ | 14 | ADC2 + RTC; active-high wake |
| SX1262 BUSY | 15 | ADC2 + RTC; digital input |
| microSD CS | 16 | ADC2 + RTC; digital output |
| SX1262 RESET | 17 | ADC2 + RTC; digital output |
| SOS button | 18 | ADC2 + RTC; active-high wake, external pulldown |
| Native USB D- | 19 | USB; do not repurpose |
| Native USB D+ | 20 | USB; do not repurpose |
| PTT button | 21 | RTC; active-high wake, external pulldown |
| GPIO26..37 | 26..37 | Reserved by WROOM-1-N16R8 flash/PSRAM; do not route |
| WM8962 I2C SDA | 38 | GPIO Matrix I2C |
| Addressable RGB | 39 | JTAG-capable after boot; one-wire data |
| Haptic enable | 40 | JTAG-capable after boot; active-high driver enable |
| BATTERY_CHARGE_ESTIMATE_LED | 41 | JTAG-capable after boot; heuristic only |
| MRAM CS | 42 | JTAG MTMS; exclusive MR25H256 chip-select while firmware runs |
| GNSS TX | 43 | UART0 default pin; remapped Serial output |
| GNSS RX | 44 | UART0 default pin; remapped Serial input |
| GPIO45 | 45 | **Strapping; do not use for LED** |
| GPIO46 | 46 | **Strapping; do not use for LED** |
| I2C SCL | 48 | GPIO Matrix I2C; no dedicated RX LED |
| Buzzer | 47 | active-high; passive buzzer requires driver/PWM as applicable |

## Auxiliary field controls

The current design package has no fabrication-confirmed routed PTT/SOS/battery/LED nets. Rev-C firmware assigns:
- GPIO21: PTT, active-high RTC wake input
- GPIO18: SOS, active-high RTC wake input
- GPIO43/44: NEO-M9N UART TX/RX
- GPIO1: battery ADC input
- GPIO47: active-high buzzer output
- GPIO39: addressable RGB data output
- GPIO40: active-high haptic-driver enable
- GPIO41: `BATTERY_CHARGE_ESTIMATE_LED` output (heuristic only; no charger STAT input)
- GPIO42: MR25H256 MRAM chip-select; no longer a TX indicator
- GPIO9: GNSS 1-PPS timing input; not reserved for future analog devices
- GPIO48: I2C SCL

For each PTT/SOS input, populate an external 47 kOhm pulldown to GND and a
normally-open pushbutton to 3V3. Add a local 100 nF capacitor from each input to GND.
This is intentionally active-high; an external pullup would invert the idle/push polarity.
The external pulldown is required for a deterministic deep-sleep state.

### GPIO42 MRAM contract

GPIO42 is exclusively assigned to the Everspin MR25H256 SPI MRAM chip-select. It is not a
TX indicator and must not be reused by another active subsystem. The addressable RGB LED on
GPIO39 remains the firmware TX/RX indication mechanism. GPIO19/20 remain the native USB
D-/D+ debug path; GPIO42's MTMS/JTAG function is therefore unavailable while MRAM CS is
driven by firmware.

### ADC2 + Wi-Fi rule

ESP32-S3 ADC2 channels must not be treated as general-purpose analog inputs while Wi-Fi is
active. The restriction is on the **ADC operation**, not on digital GPIO operation. Using
GPIO11..18 for SPI, radio control and buttons as digital signals is valid while Wi-Fi is
running. Future analog sensors should use another verified ADC1 route or an external ADC;
GPIO9 is occupied by GNSS 1-PPS.

## WM8962 analogue net contract

The current design package does not contain a fabrication-confirmed native KiCad netlist, so firmware locks only to the documented Rev-C audio contract:

- `MIC_L/MIC_R` -> WM8962 `IN1L/IN1R` (PGA path, recommended microphone pins).
- `LINE2_L/LINE2_R` -> WM8962 `IN2L/IN2R` (direct input-mixer path).
- `LINE3_L/LINE3_R` -> WM8962 `IN3L/IN3R` (direct input-mixer path).
- ESP32-S3 `I2S_DOUT` GPIO6 -> WM8962 `DACDAT`; ESP32-S3 `I2S_DIN` GPIO7 <- WM8962 `ADCDAT`.
- Playback is locked to WM8962 `DACL -> HPOUTL` and `DACR -> HPOUTR` using the codec's direct DAC path. Speaker mixers remain disabled because the supplied PCB package does not prove a speaker-load net.

Do not enable speaker routing until the final schematic/netlist proves `SPKOUTL/R` connectivity, load impedance, and the required Class-D power network.

## Deep-sleep wake contract

ESP32-S3 deep-sleep wake is configured with EXT1/ANY_HIGH on GPIO14 (SX1262 DIO1),
GPIO21 (PTT), and GPIO18 (SOS). All three are RTC-capable and non-strapping GPIOs.
The button inputs must have external bias because internal GPIO pulls are not relied upon
while the RTC power domain is reduced.

SX1262 DIO1 is connected directly to ESP32-S3 GPIO14. No transistor inverter is required:
the SX1262 DIO1 interrupt is active-high and the radio/MCU logic are 3.3 V compatible.
The radio remains powered while the MCU enters deep sleep and is placed into SX1262 receive
duty-cycle mode rather than standby/sleep. A received packet asserts DIO1 and wakes the
ESP32-S3. The duty-cycle receiver requires a sufficiently long transmitter preamble; this
firmware uses a 32-symbol LoRa preamble and RadioLib's automatic duty-cycle timing.

## GNSS 1-PPS and periodic time synchronization

- NEO-M9N 1-PPS is routed to **GPIO9** and configured as a rising-edge interrupt input.
- The PPS edge timestamp is exposed in runtime GNSS state and is considered valid for 2 seconds.
- The system clock is synchronized from validated GNSS UTC time at boot/first valid fix and
  at least once every 12 hours while the firmware remains running.
- Deep sleep also arms a **12-hour RTC timer wake** so an idle device periodically boots,
  reacquires GNSS time, and performs the same synchronization path. GPIO wake sources remain
  available in parallel.
- The PCB must provide a clean 3.3 V-compatible PPS signal from the GNSS module; no LED,
  analog sensor, or external pull network should be attached to GPIO9 that can distort PPS.

## I2C battery-gauge polling

The MAX17048 at I2C address `0x36` is polled every 10 seconds on the shared I2C bus. A bounded
50 ms I2C timeout and a mutex serialize access to the bus. VCELL and SOC are validated before
they replace the ADC-derived battery state. If the gauge is absent or becomes unresponsive,
the existing ADC1 battery measurement remains the fallback.
