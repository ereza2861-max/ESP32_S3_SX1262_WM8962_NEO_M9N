# PCB mapping source of truth — ESP32-S3-WROOM-1-N16R8

Mapping berikut adalah target mapping firmware ESP32-S3-WROOM-1-N16R8 dan harus tetap identik dengan `include/BoardConfig.h`. Ini membutuhkan rerouting PCB dari mapping ESP32-WROOM-32E lama.

- GPIO12/13/11: shared SPI (SCK/MISO/MOSI)
- GPIO10: SX1262 NSS
- GPIO14: SX1262 RESET
- GPIO2: SX1262 DIO1 / IRQ (direct, active-high)
- GPIO18: SOS button, active-high RTC wake input
- GPIO21: PTT button, active-high RTC wake input
- GPIO15: SX1262 BUSY (mandatory)
- GPIO16: microSD CS
- GPIO8/9: WM8962 I2C
- GPIO4: WM8962 BCLK
- GPIO5: WM8962 LRCLK
- GPIO6: ESP32-S3 -> WM8962 DACDAT
- GPIO7: WM8962 ADCDAT -> ESP32-S3
- GPIO19/20: native USB D-/D+
- GPIO43/44: NEO-M9N UART TX/RX (moved off RTC GPIO18 to free SOS wake)
- external 24 MHz oscillator: WM8962 MCLK

Mapping Rev-B sekarang menetapkan pin fisik untuk PTT/SOS, tetapi net tersebut tetap
harus benar-benar dirutekan pada PCB. Kontrol web/API tetap dapat digunakan sebagai
jalur kontrol sekunder.


## Auxiliary field controls (required PCB reroute)

The supplied PCB has no routed PTT/SOS/battery/LED nets. Rev-B firmware assigns:
- GPIO21: PTT, active-high RTC wake input
- GPIO18: SOS, active-high RTC wake input
- GPIO43/44: NEO-M9N UART TX/RX after moving GNSS off GPIO18
- GPIO1: battery ADC input
- GPIO48: status LED output

For each PTT/SOS input, populate an external 47 kOhm pulldown to GND and a
normally-open pushbutton to 3V3. Add a local 100 nF capacitor from the MCU input
to GND. This is intentionally active-high; an external pullup would produce the
opposite idle/push polarity and is not electrically consistent with active-high
wake. The external pulldown is required for a deterministic deep-sleep state.

PTT/SOS and the GNSS reroute are firmware-safe only if the Rev-B PCB actually
routes these nets. Do not install this mapping onto the existing unrouted PCB.


## WM8962 analogue net contract

The supplied PCB mapping does not contain a native KiCad netlist, so firmware locks only to the documented Rev B audio contract:

- `MIC_L/MIC_R` -> WM8962 `IN1L/IN1R` (PGA path, recommended microphone pins).
- `LINE2_L/LINE2_R` -> WM8962 `IN2L/IN2R` (direct input-mixer path).
- `LINE3_L/LINE3_R` -> WM8962 `IN3L/IN3R` (direct input-mixer path).
- ESP32-S3 `I2S_DOUT` GPIO6 -> WM8962 `DACDAT`; ESP32-S3 `I2S_DIN` GPIO7 <- WM8962 `ADCDAT`.
- Playback is locked to WM8962 `DACL -> HPOUTL` and `DACR -> HPOUTR` using the codec's direct DAC path. Speaker mixers remain disabled because the supplied PCB package does not prove a speaker-load net.

Do not enable speaker routing until the final schematic/netlist proves `SPKOUTL/R` connectivity, load impedance, and the required Class-D power network.


## Deep-sleep wake contract

ESP32-S3 deep-sleep wake is configured with EXT1/ANY_HIGH on GPIO2 (SX1262 DIO1),
GPIO21 (PTT), and GPIO18 (SOS). ESP32-S3 RTC deep-sleep wake GPIOs are GPIO0..21;
GPIO47 is therefore not a valid deep-sleep GPIO wake source. The button inputs
must have external bias because internal GPIO pulls are not relied upon while
the RTC power domain is reduced.

SX1262 DIO1 is connected directly to ESP32-S3 GPIO2. No transistor inverter is
required: the SX1262 DIO1 interrupt is active-high and the radio/MCU logic are
3.3 V compatible. The radio must remain powered and in receive mode while the
MCU enters deep sleep; do not put the SX1262 into standby/sleep if DIO1 is the
intended wake source. SX1262 packet IRQ status remains asserted until serviced,
so a received packet can wake the MCU. The PCB should still provide a clean
local decoupling network for the radio and controlled routing of DIO1.
