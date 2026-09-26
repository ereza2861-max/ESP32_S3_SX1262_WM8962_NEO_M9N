# PIN-MAPPING.md

## Source-level contract

This document records the immutable source-level mapping requested for the ESP32-C3
sensor node reconstruction. The mapping is **NOT PHYSICALLY VALIDATED**.

| Function | GPIO |
|---|---:|
| Profile selector bit0 | 4 |
| Profile selector bit1 | 5 |
| Profile selector bit2 (reserved) | 6 |
| I2C SDA | 8 |
| I2C SCL | 9 |
| Button / long-press | 10 |
| Wind pulse | 11 |
| MFRC522 CS | 7 |
| MFRC522 SCK | 3 |
| MFRC522 MOSI | 2 |
| MFRC522 MISO | 1 |
| RFID RST | 20 |
| Buzzer | 21 |
| UART1 RX | 18 |
| UART1 TX | 19 |
| Profile 2/3 time-share | 15 |

## Reconstruction behavior

- The selector is sampled once at boot from three inputs.
- Encodings `0..3` map to the four currently defined profiles.
- Encodings `4..7` are reserved and fail closed to profile 0.
- GPIO15 is used by the source-level Profile 2/3 ADC/OneWire/SPI arbitration
  contract, replacing the previous GPIO3 time-share contract.
- MFRC522 SPI pins follow the requested source-level contract.

## Hardware validation blockers

The requested mapping must not be treated as electrically proven merely because
the firmware compiles.

The current repository targets **ESP32-C3**. The Espressif ESP32-C3 datasheet
identifies GPIO2, GPIO8 and GPIO9 as boot strapping pins, GPIO4..GPIO7 as JTAG-
related pins, GPIO11 as the VDD_SPI supply pin, and GPIO12..GPIO17 as SPI0/1
flash-related/recommended-for-flash pins. It also identifies GPIO5 as ADC2_CH0;
GPIO15 is not the documented ADC2_CH0 pin.

Therefore:

1. **GPIO15 time-share is a source-level reconstruction of the requested intent,
   not a verified ESP32-C3 ADC pin assignment.** The repository must not claim
   that GPIO15 is ADC2_CH0 until the exact silicon/module and board design are
   independently verified.
2. **GPIO11 wind-pulse use remains hardware-dependent** because the ESP32-C3
   datasheet assigns this pin to VDD_SPI unless the flash power arrangement
   permits GPIO use.
3. **GPIO4 selector bit0 overlaps the existing Profile 0 turbidity ADC pin.**
   The selector is sampled before runtime use, but a solder jumper remaining
   electrically attached makes this an unresolved board-level conflict.
   No alternative sensor pin is invented here because the source requirements
   do not establish one.
4. **GPIO2 is a strapping pin.** The requested MFRC522 MOSI assignment is only
   safe if the attached circuit guarantees the required boot level and the pin
   is subsequently used as an output after boot.
5. GPIO18/19, GPIO20/21 and GPIO4..7 have additional USB/JTAG/UART0
   multiplexing considerations. Their runtime GPIO-matrix use remains a board
   integration item.

### Status

- Source-level implementation: **IMPLEMENTED**
- Physical PCB/electrical validation: **NOT VERIFIED**
- GPIO15 ADC capability on ESP32-C3: **UNRESOLVED / CONFLICTS WITH THE REQUESTED CLAIM**
- GPIO11 electrical suitability: **NOT VERIFIED**
- GPIO4 selector/turbidity coexistence: **UNRESOLVED**
