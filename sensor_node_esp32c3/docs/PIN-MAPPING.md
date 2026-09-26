# ESP32-C3 Sensor Node — Pin Mapping

This document is the source-level pin contract for the current runtime-profile
architecture. Hardware validation is separate and remains required where noted.

| Function | GPIO | Status / constraint |
|---|---:|---|
| I2C SDA | 8 | HARDWARE DEPENDENT: strapping pin; normal I2C pull-up required |
| I2C SCL | 9 | HARDWARE DEPENDENT: strapping pin; normal I2C pull-up required |
| Button / long-press | 10 | Source-level assignment |
| Wind pulse (Profile 2) | 11 | HARDWARE DEPENDENT: VDD_SPI role must be verified |
| MFRC522 CS | 7 | Dedicated RFID CS |
| MFRC522 SCK | 6 | Dedicated RFID SCK |
| MFRC522 MOSI | 2 | HARDWARE DEPENDENT: strapping pin; external 10 kOhm pull-up to 3.3 V; output after boot |
| MFRC522 MISO | 1 | Source-level assignment |
| RFID RST | 20 | Source-level assignment |
| Buzzer | 21 | LEDC output |
| UART1 RX | 18 | Placeholder protocol path |
| UART1 TX | 19 | Placeholder protocol path |
| GPIO3 mux COM | 3 | CD74HC4051 COM; active-profile ADC / OneWire / ADXL355 CS path |
| 4051 S0 | 0 | Selector bit 0; reserved exclusively for GPIO3 mux |
| 4051 S1 | 5 | Selector bit 1; reserved exclusively for GPIO3 mux |
| 4051 S2 | GND | Fixed LOW on PCB; no ESP32 GPIO consumed |
| 4051 /E | GND | Fixed enabled on PCB; no ESP32 GPIO consumed |

## GPIO3 mux / arbitration

GPIO3 is the COM pin of an external CD74HC4051. The PCB routes the shared
functions as follows:

| 4051 channel | Function | Selector S1:S0 |
|---:|---|---:|
| Y0 | OneWire | 00 |
| Y1 | ADC | 01 |
| Y2 | ADXL355 CS | 10 |
| Y3 | Reserved | 11 |

S2 and /E are tied to GND on the PCB, so only GPIO0 (S0) and GPIO5 (S1) are
consumed by the mux. The firmware still serializes access to GPIO3 with a
FreeRTOS mutex and selects the branch before every driver read. During driver
registration/rebuild it also selects the branch before a driver `begin()` so
OneWire device discovery can occur through the mux.

The OneWire branch requires an external 4.7 kOhm pull-up on Y0. The ADXL355 CS
branch should have an external 10 kOhm pull-up on Y2 so CS remains deasserted
when that channel is not selected. Add local 100 kOhm pulldowns on S0/S1 if the
PCB needs a deterministic mux state during ESP32-C3 reset; firmware cannot
guarantee selector levels before GPIO initialization.

The cross-profile overlap is intentional and legal: only one profile PCB/cable
set is installed at a time. The mux exists because GPIO3 is overlapped within
the selected profile's own sensor roster.

Electrical validation remains required for CD74HC4051 on-resistance, ADC source
impedance/accuracy, OneWire rise/fall timing, and ADXL355 CS timing.

## ADC and strapping constraints

The ESP32-C3 ADC1 channels used by the profile contract are GPIO3 and GPIO4.
ADC2 availability is not relied upon by the profile roster.

GPIO2, GPIO8 and GPIO9 are strapping-sensitive assignments. GPIO11 has a
hardware-dependent VDD_SPI role. These items cannot be marked VERIFIED by a
firmware build alone.

## SPI and UART

MFRC522 uses the dedicated CS/SCK/MOSI/MISO/RST assignments above. Profile 2
and Profile 3 use GPIO3 for the ADXL355 CS placeholder; Profile 3 also uses
GPIO11 for the MAX31865 CS placeholder.

UART/SDI-12 protocol handling remains PLACEHOLDER and requires electrical and
protocol validation on the target hardware.
