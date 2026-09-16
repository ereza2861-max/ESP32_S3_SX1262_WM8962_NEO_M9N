# PCB mapping source of truth — ESP32-S3-WROOM-1-N16R8

Mapping berikut adalah **source of truth Rev-C** untuk routing firmware ESP32-S3-WROOM-1-N16R8.
Nilai pin harus identik dengan `include/BoardConfig.h`; jangan membuat alias pin
alternatif di schematic/KiCad tanpa mengubah kedua dokumen dan firmware secara
atomik. Semua net pada tabel di bawah dianggap komitmen routing Rev-C. Ini membutuhkan rerouting PCB dari mapping ESP32-WROOM-32E lama.

Rev-C pin correction: I2C SDA is moved from GPIO8 to GPIO38; the microSD CS net is moved from GPIO38 to GPIO16; MAX2016 reflected detector is moved from GPIO16 to GPIO8. GPIO16 is digital-only in firmware and is never used as an ADC input.

- GPIO12/13/11: shared SPI (SCK/MISO/MOSI)
- GPIO10: SX1262 NSS
- GPIO17: SX1262 RESET
- GPIO14: SX1262 DIO1 / IRQ (direct, active-high, RTC-capable)
- GPIO18: SOS button, active-high RTC wake input
- GPIO21: PTT button, active-high RTC wake input
- GPIO15: SX1262 BUSY (mandatory)
- GPIO16: microSD CS (secondary SPI chip-select; digital only)
- GPIO38: WM8962 I2C SDA
- GPIO9: WM8962 I2C SCL
- GPIO4: WM8962 BCLK
- GPIO5: WM8962 LRCLK
- GPIO6: ESP32-S3 -> WM8962 DACDAT
- GPIO7: WM8962 ADCDAT -> ESP32-S3
- GPIO19/20: native USB D-/D+
- GPIO43/44: NEO-M9N UART TX/RX (moved off RTC GPIO18 to free SOS wake)
- external 24 MHz oscillator: WM8962 MCLK
- GPIO1: battery ADC (ADC1)
- GPIO2: MAX2016 forward detector ADC (ADC1)
- GPIO8: MAX2016 reflected detector ADC (ADC1)

Mapping Rev-C sekarang menetapkan pin fisik untuk PTT/SOS, tetapi net tersebut tetap
harus benar-benar dirutekan pada PCB. Kontrol web/API tetap dapat digunakan sebagai
jalur kontrol sekunder.


## Auxiliary field controls (required PCB reroute)

The supplied PCB has no routed PTT/SOS/battery/LED nets. Rev-C firmware assigns:
- GPIO21: PTT, active-high RTC wake input
- GPIO18: SOS, active-high RTC wake input
- GPIO43/44: NEO-M9N UART TX/RX after moving GNSS off GPIO18
- GPIO1: battery ADC input
- GPIO47: active-high buzzer output
- GPIO39: addressable RGB data output
- GPIO40: active-high haptic-driver enable
- GPIO41: charging-indicator output (heuristic only; no charger STAT input)
- GPIO42: TX indicator output
- GPIO48: RX indicator output

For each PTT/SOS input, populate an external 47 kOhm pulldown to GND and a
normally-open pushbutton to 3V3. Add a local 100 nF capacitor from the MCU input
to GND. This is intentionally active-high; an external pullup would produce the
opposite idle/push polarity and is not electrically consistent with active-high
wake. The external pulldown is required for a deterministic deep-sleep state.

PTT/SOS and the GNSS reroute are firmware-safe only if the Rev-C PCB actually
routes these nets. Do not install this mapping onto the existing unrouted PCB.


## WM8962 analogue net contract

The supplied PCB mapping does not contain a native KiCad netlist, so firmware locks only to the documented Rev-C audio contract:

- `MIC_L/MIC_R` -> WM8962 `IN1L/IN1R` (PGA path, recommended microphone pins).
- `LINE2_L/LINE2_R` -> WM8962 `IN2L/IN2R` (direct input-mixer path).
- `LINE3_L/LINE3_R` -> WM8962 `IN3L/IN3R` (direct input-mixer path).
- ESP32-S3 `I2S_DOUT` GPIO6 -> WM8962 `DACDAT`; ESP32-S3 `I2S_DIN` GPIO7 <- WM8962 `ADCDAT`.
- Playback is locked to WM8962 `DACL -> HPOUTL` and `DACR -> HPOUTR` using the codec's direct DAC path. Speaker mixers remain disabled because the supplied PCB package does not prove a speaker-load net.

Do not enable speaker routing until the final schematic/netlist proves `SPKOUTL/R` connectivity, load impedance, and the required Class-D power network.


## Deep-sleep wake contract

ESP32-S3 deep-sleep wake is configured with EXT1/ANY_HIGH on GPIO14 (SX1262 DIO1),
GPIO21 (PTT), and GPIO18 (SOS). GPIO14 is an RTC-capable, non-strapping GPIO.
The button inputs must have external bias because internal GPIO pulls are not
relied upon while the RTC power domain is reduced.

SX1262 DIO1 is connected directly to ESP32-S3 GPIO14. No transistor inverter is
required: the SX1262 DIO1 interrupt is active-high and the radio/MCU logic are
3.3 V compatible. The radio remains powered while the MCU enters deep sleep and
is placed into SX1262 receive duty-cycle mode rather than standby/sleep. A
received packet asserts DIO1 and wakes the ESP32-S3. The duty-cycle receiver
requires a sufficiently long transmitter preamble; this firmware uses a
32-symbol LoRa preamble and RadioLib's automatic duty-cycle timing. The PCB
must provide continuous radio power, clean local decoupling, and controlled
routing of DIO1.
