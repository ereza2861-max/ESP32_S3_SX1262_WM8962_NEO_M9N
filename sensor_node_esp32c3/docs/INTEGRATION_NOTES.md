# ESP32-C3 Sensor Node — Integration Notes

This document records every architectural decision, deliberate trade-off, and
`NOT VERIFIED` item left open by the STEP 1..8 patch series. It is the
companion to `README.md` and is the reference for future audits.

## Decision log

### D-01 — Profile alternation by physical cable swap

Only one profile's sensors are physically installed at a time. Pin overlap
across profiles is therefore legal. The MFRC522 RFID reader is the single
exception: it is present in every profile and must never be overlapped.

### D-02 — Legacy battery ADC is not part of Profile 0

ESP32-C3 has only two usable ADC1 channels after DIP (GPIO0/1), strapping
(GPIO2), and MFRC522/I2C/UART/pulse reservations. Profile 0 uses both
channels for `water_wave` (GPIO3) and `water_turbidity` (GPIO4). Battery
monitoring is not available in Profile 0. The legacy battery ADC pin remains
available only for the fallback `SensorsExample` roster.

### D-03 — Rain gauge is I2C

The tipping-bucket rain gauge in Profile 1 and Profile 2 is registered as a
digital I2C device instead of a pulse input. This eliminates the need for a
dedicated pulse pin. `RAIN_GAUGE_I2C_ADDRESS = 0x28` is a placeholder.

### D-04 — Profile 2 GPIO3 time-share (ADC + SPI CS)

GPIO3 is shared between the wind vane ADC input and the ADXL355 chip select.
The ESP32-C3 GPIO matrix cannot make one pin simultaneously an ADC input and
a digital output. The contract in `ProfileSensorsContract` (in
`ProfileSensors.h`) is a statement, not a scheduler; the STEP 8 integration
owns the actual serialization.

### D-05 — Profile 3 GPIO3 time-share (OneWire + SPI CS), leakage accepted

GPIO3 is shared between the optional DS18B20 OneWire bus and the ADXL355
chip select. The OneWire bus has an external 4.7 kOhm pull-up to 3.3 V that
stays connected when ADXL355 CS is driven LOW, causing ~0.70 mA leakage and
raising the CS low level above 0 V. The user accepted this trade-off
explicitly. Whether ADXL355 CS is still recognized is `UNVERIFIED BEHAVIOR`.

### D-06 — Profile 3 interface substitutions

Profile 3 uses digital/I2C variants for sensors that would otherwise need
dedicated ADC or pulse pins:

- Wind anemometer: digital UART/SDI-12 (not pulse).
- Snow depth: ultrasonic I2C (MaxBotix I2CXL class, not trigger-echo).
- O2: industrial I2C sensor (not analog).
- Pyranometer: UART/SDI-12.

Only MAX31865 needs a dedicated CS pin, and it reuses GPIO11 because
Profile 2 and Profile 3 are physically alternated.

### D-07 — OTA is ESP32-C3 only, AP-only

The sensor node no longer connects to an external router for OTA. OTA is
served through a local AP triggered by a physical button long-press. The AP
is open (no Wi-Fi password) and bounded to a 10-minute window. Firmware
upload requires the OTA password provisioned over serial. The AP does not
accept a new OTA password. The ESP32-S3 has no OTA by design.

## Placeholder drivers (`QUALITY_STALE` until hardware-specific code exists)

| Sensor | Profile | Placeholder interface |
|---|---|---|
| MAX31865 (PT100) | 3 | GenericI2c (SPI register read not implemented) |
| VEML6075 (UV) | 3 | GenericI2c |
| Snow depth ultrasonic | 3 | GenericI2c |
| O2 industrial | 3 | GenericI2c |
| PMS5003 (PM2.5/PM10) | 2 | GenericUart (32-byte protocol not implemented) |
| SCD41 CO2 | 1, 2 | GenericI2c (Sensirion command protocol not implemented) |
| TEROS 12 soil moisture | 1 | GenericUart (SDI-12 not implemented) |
| Pyranometer | 1, 3 | GenericUart (SDI-12 not implemented) |
| Wind anemometer | 3 | GenericUart (digital protocol not implemented) |
| Rain gauge I2C | 1, 2 | GenericI2c (register protocol not implemented) |
| ADXL355 tilt/seismic | 2, 3 | GenericI2c (SPI register driver not implemented) |

## NOT VERIFIED items

1. All GPIO assignments in `ProfileConfig.h`. No PCB exists yet.
2. All I2C addresses for non-BME280 devices.
3. Profile 2 GPIO3 time-share scheduler.
4. Profile 3 GPIO3 leakage tolerance by ADXL355 CS.
5. Binary size headroom under `app0 = 0x190000`.
6. `attachInterruptArg` availability on the actual Arduino-ESP32 core (3.x
   assumed, not verified by build).
7. Rain gauge I2C address `0x28`.
8. Native test execution (requires a PlatformIO native env on the build host).
9. WebServer/Update library behavior under concurrent ArduinoOTA + WebUI
   upload on the same AP.

## NOT APPLICABLE items

1. ESP32-S3 OTA — explicitly out of scope by design.
2. `shared/SensorProtocol.h` — unchanged by this patch series.
3. `partitions.csv` — unchanged by this patch series.
4. Historical patch artifacts — not modified by this patch series.

## Open corrections (pending explicit decisions)

1. Partition layout if `pio run` shows firmware > 1.6 MB. Options:
   - Reduce SPIFFS, enlarge `app0`/`app1`.
   - Remove SPIFFS entirely (WebUI is embedded in PROGMEM).
   - These are separate decisions, not taken here.
2. `MAX_SENSORS = 12` has no headroom for Profile 1. Raising it requires
   a separate RAM budget decision.
3. Specific drivers for placeholder sensors, one per hardware model.
4. `build_src_filter` for the native registry test may need adjustment
   depending on PlatformIO version.
5. `__builtin_nanf("")` in the registry test is GCC/Clang-specific; on MSVC
   use `std::nanf("")` instead.
