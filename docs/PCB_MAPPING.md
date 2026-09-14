# PCB mapping source of truth — ESP32-S3-WROOM-1-N16R8

Mapping berikut adalah target mapping firmware ESP32-S3-WROOM-1-N16R8 dan harus tetap identik dengan `include/BoardConfig.h`. Ini membutuhkan rerouting PCB dari mapping ESP32-WROOM-32E lama.

- GPIO12/13/11: shared SPI (SCK/MISO/MOSI)
- GPIO10: SX1262 NSS
- GPIO14: SX1262 RESET
- GPIO2: SX1262 DIO1 / IRQ
- GPIO15: SX1262 BUSY (mandatory)
- GPIO16: microSD CS
- GPIO8/9: WM8962 I2C
- GPIO4: WM8962 BCLK
- GPIO5: WM8962 LRCLK
- GPIO6: ESP32-S3 -> WM8962 DACDAT
- GPIO7: WM8962 ADCDAT -> ESP32-S3
- GPIO19/20: native USB D-/D+
- external 24 MHz oscillator: WM8962 MCLK

Tidak ada pin dedicated button/PTT/SOS atau battery ADC pada mapping yang diberikan,
sehingga kontrol PTT/SOS dipertahankan melalui web API dan dapat ditambahkan kemudian
melalui GPIO yang benar-benar dirutekan pada revisi PCB.


## Auxiliary field controls (required PCB reroute)

The supplied PCB has no routed PTT/SOS/battery/LED nets. The firmware patch assigns:
- GPIO21: PTT, active-low with INPUT_PULLUP
- GPIO47: SOS, active-low with INPUT_PULLUP
- GPIO1: battery ADC input
- GPIO48: status LED output

These assignments are firmware-safe only if the PCB revision actually routes those
nets and does not reuse them elsewhere. Do not install this mapping onto the
existing unrouted PCB and expect the controls to work.


## WM8962 analogue net contract

The supplied PCB mapping does not contain a native KiCad netlist, so firmware locks only to the documented Rev B audio contract:

- `MIC_L/MIC_R` -> WM8962 `IN1L/IN1R` (PGA path, recommended microphone pins).
- `LINE2_L/LINE2_R` -> WM8962 `IN2L/IN2R` (direct input-mixer path).
- `LINE3_L/LINE3_R` -> WM8962 `IN3L/IN3R` (direct input-mixer path).
- ESP32-S3 `I2S_DOUT` GPIO6 -> WM8962 `DACDAT`; ESP32-S3 `I2S_DIN` GPIO7 <- WM8962 `ADCDAT`.
- Playback is locked to WM8962 `DACL -> HPOUTL` and `DACR -> HPOUTR` using the codec's direct DAC path. Speaker mixers remain disabled because the supplied PCB package does not prove a speaker-load net.

Do not enable speaker routing until the final schematic/netlist proves `SPKOUTL/R` connectivity, load impedance, and the required Class-D power network.
