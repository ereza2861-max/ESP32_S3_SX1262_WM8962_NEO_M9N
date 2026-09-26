# ESP32-C3 Sensor Node — Integration Notes

This file records final architecture decisions and validation status. Design
decisions are closed; remaining items are hardware/build verification.

## Decision log

### D-01 — Runtime profile selection
Profile selection uses the `sensor/profile` NVS key and WebUI `POST /profile`.
There is no physical selector. A profile change requires confirmation that the
physical sensor cabling was replaced, then the firmware persists the new profile
and reboots.

### D-02 — Immutable profile rosters
The source-level roster counts are immutable: Profile 0 = 6, Profile 1 = 12,
Profile 2 = 8, Profile 3 = 9. Cross-profile pin overlap is intentional because
only one profile's sensor cabling is installed at a time. MFRC522 is global and
never overlaps.

### D-03 — GPIO3 CD74HC4051 mux
GPIO3 is the COM pin of a CD74HC4051. Y0=OneWire, Y1=ADC, Y2=ADXL355 CS,
and Y3 is reserved. S0=GPIO0 and S1=GPIO5; S2 and /E are tied to GND on the
PCB. `Gpio3TransactionGuard` still serializes access with a FreeRTOS mutex,
selects the required mux branch, and establishes the pin mode/settle interval
before the driver read. Driver rebuild also selects the branch before `begin()`
so OneWire discovery occurs through the mux.

The OneWire branch requires an external 4.7 kOhm pull-up on Y0 and the ADXL355
CS branch should have an external 10 kOhm pull-up on Y2. The mux arrangement
removes the previous direct OneWire/ADXL355 electrical overlap. CD74HC4051
on-resistance, ADC accuracy/source impedance, OneWire timing, and ADXL355 CS
timing remain **NOT VERIFIED** electrically.

### D-04 — GPIO11 hardware dependency
GPIO11 remains the Profile 2 wind-pulse pin and Profile 3 MAX31865 CS. Its use as
a normal GPIO is **HARDWARE DEPENDENT** because of the ESP32-C3 VDD_SPI role.

### D-05 — Strapping pins
GPIO2 is MFRC522 MOSI and requires an external 10 kOhm pull-up to 3.3 V.
GPIO8/9 are I2C SDA/SCL and require normal I2C pull-ups. These are electrical
constraints, not firmware-only guarantees.

### D-06 — Placeholder addresses
Locked placeholder addresses include rain gauge `0x28`, VEML6075 `0x10`,
O2 `0x73`, snow-depth `0x70`, BME280 `0x76`, Atlas EZO-EC `0x64`,
Atlas EZO-pH `0x63`, and SCD4x `0x62`. They are **PLACEHOLDER** until hardware
verification.

### D-07 — RFID global event
The MFRC522 event descriptor is `0x00F0`, flagged
`ENABLED | EVENT_DRIVEN | READ_ONLY`. A new tag updates its value to `1.0` with
`QUALITY_VALID` and triggers a buzzer pulse.

### D-08 — OTA AP-only
OTA is ESP32-C3-only and AP-only. The AP window is a hard 10 minutes and is not
extended by client activity. Upload requires the OTA password already stored
in NVS. The WebUI cannot replace that password and there is no STA-mode OTA.

### D-09 — Registry capacity
`MAX_SENSORS = 13` and `MAX_DRIVERS = 13`. The capacity contract covers the
largest 12-sensor profile and the global RFID descriptor.

### D-10 — Generic I2C placeholder
Generic I2C is intentionally a placeholder. It does not perform raw register transactions or expose
raw register bytes as telemetry; reads return `QUALITY_STALE` until a sensor-specific measurement
protocol is implemented.

### D-11 — Partition and binary size
`app0 = 0x190000` and `app1 = 0x190000` remain unchanged. Binary size headroom is
**NOT VERIFIED** until the target build is executed.

## Placeholder driver table

| Sensor | Profile | Interface | Status |
|---|---:|---|---|
| MAX31865 | 3 | SPI/CS GPIO11 | PLACEHOLDER |
| VEML6075 | 3 | I2C | PLACEHOLDER |
| Snow depth | 3 | I2C | PLACEHOLDER |
| O2 | 3 | I2C | PLACEHOLDER |
| PMS5003 | 2 | UART1 | PLACEHOLDER |
| SCD4x | 1/2 | I2C | PLACEHOLDER |
| TEROS 12 | 1 | UART | PLACEHOLDER |
| Pyranometer | 1/3 | UART | PLACEHOLDER |
| Wind anemometer | 3 | UART | PLACEHOLDER |
| Rain gauge | 1/2 | I2C 0x28 | PLACEHOLDER |
| ADXL355 | 2/3 | SPI, CS via GPIO3 mux | PLACEHOLDER |

## Validation status

- Source-level profile/NVS/WebUI implementation: **IMPLEMENTED**
- GPIO3 CD74HC4051 mux arbitration: **IMPLEMENTED / NOT VERIFIED electrically**
- OneWire per-pin pool and shared GPIO3 use: **IMPLEMENTED**
- RFID global event and buzzer: **IMPLEMENTED**
- OTA AP-only architecture: **IMPLEMENTED**
- Native tests: **IMPLEMENTED**
- Target firmware build: **NOT VERIFIED**
- Native test execution: **NOT VERIFIED** until PlatformIO native toolchain is run
- HIL/electrical validation: **NOT VERIFIED**
- GPIO11 suitability: **HARDWARE DEPENDENT**
- Locked sensor addresses: **PLACEHOLDER**
