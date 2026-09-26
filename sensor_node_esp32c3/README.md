# FieldRadio ESP32-C3 BLE Sensor Node

PlatformIO project for the BLE peripheral/counterpart of the gateway `SensorProtocol.h`.
The canonical wire contract is `../shared/SensorProtocol.h`; do not create a second copy.

## Profile hardware contract

The sensor node has four mutually exclusive profiles selected at boot by a three-bit static selector.
Production uses solder jumpers; the prototype used a profile selector during development. The selector is
sampled once at boot and is never hot-switched.
Only one profile's sensor cabling is installed at a time; changing profile means physically
unplugging the previous profile's sensor cables and installing the new profile's PCB/cabling.
Pin overlap between different profiles is therefore intentional. The MFRC522 RFID reader is the
shared exception and must not be overlapped by profile-specific wiring.

Profile rosters are source-level contracts and are not physical validation evidence. Sensor models,
I2C addresses, UART/SDI-12 protocol variants, ADC calibration, and time-shared GPIO behavior still
require hardware verification where marked as placeholders.

- **Profile 0 — Island/Sea:** six sensors; GPIO3 is the wave ADC input; the legacy GPIO4 turbidity path remains hardware-dependent because GPIO4 is also selector bit0 used by the
  wave and turbidity inputs. Legacy battery monitoring is not part of this profile.
- **Profile 1 — Tropical Forest:** twelve profile sensors; three DS18B20 channels share
  OneWire GPIO3 and the rain gauge uses the I2C bus at the locked placeholder address
  `RAIN_GAUGE_I2C_ADDRESS = 0x28`.
- **Profile 2 — Volcanic Mountain:** eight sensors. GPIO15 is time-shared between the wind-vane
  ADC and the ADXL355 chip-select; GPIO11 is used for the wind-pulse input. The integration layer
  must serialize the two GPIO15 modes and allow the ADC to settle before sampling.
- **Profile 3 — Sub-Zero Snow:** nine sensors using UART/SDI-12 and I2C substitutions where
  dedicated ADC/pulse pins are unavailable. GPIO15 is shared by the optional DS18B20 OneWire bus and ADXL355 CS; the estimated pull-up leakage is a **hardware-validation-required** item.

No profile implementation should fabricate a sensor reading when its hardware-specific parser or
calibration is not implemented; such drivers report stale quality instead.
## ESP32-S3 LoRa/gateway OTA design

The ESP32-S3 LoRa/gateway node has NO OTA by explicit design.

## Pin map

| Function | Pin |
|---|---:|
| Profile selector bit0 | GPIO4 |
| Profile selector bit1 | GPIO5 |
| Profile selector bit2 (reserved) | GPIO6 |
| I2C SDA | GPIO8 |
| I2C SCL | GPIO9 |
| Button / long-press | GPIO10 |
| Wind pulse | GPIO11 |
| MFRC522 CS | GPIO7 |
| MFRC522 SCK | GPIO3 |
| MFRC522 MOSI | GPIO2 |
| MFRC522 MISO | GPIO1 |
| RFID RST | GPIO20 |
| Buzzer | GPIO21 |
| UART1 RX | GPIO18 |
| UART1 TX | GPIO19 |
| Profile 2/3 time-share | GPIO15 |

All assignments are a **source-level contract** and **NOT PHYSICALLY VALIDATED**.
See `PIN-MAPPING.md` for the hardware-specific caveats and unresolved ESP32-C3
pin-capability conflicts.

## Sensor profiles

| Profile | Roster count | ID range | Status |
|---|---:|---|---|
| Profile 0 — Island/Sea | 6 | 0x0100..0x0105 | Source-level roster; hardware/protocol validation remains required |
| Profile 1 — Tropical Forest | 12 | 0x0200..0x020B | Twelve profile entries; RFID uses the 13th registry slot |
| Profile 2 — Volcanic Mountain | 8 | 0x0300..0x0307 | Source-level roster; GPIO15 time-sharing requires hardware validation |
| Profile 3 — Sub-Zero Snow | 9 | 0x0400..0x0408 | Source-level roster; GPIO15 leakage requires hardware validation |

Profile 1 has 12 profile sensors. The global RFID event descriptor is registered separately, so
the registry capacity is 13 and must not be reduced below that value.

## RFID event descriptor

MFRC522 RFID is registered as the global sensor/event ID `0x00F0`. A new UID appearance updates
the event value to `1.0` with `QUALITY_VALID` and triggers the configured buzzer pulse. Repeated
polls for the same UID are suppressed by the RFID reader debounce contract.

## Button long-press

GPIO10 is the button input. A confirmed 1.5 s long press toggles the OTA
AP through OtaApManager.

## OTA over Wi-Fi AP

- Trigger: physical button long-press.
- AP: open, with a 10-minute window.
- Upload: requires the OTA password provisioned from serial.
- Partitions: unchanged; available headroom is NOT VERIFIED.
- ESP32-S3: has NO OTA.

## Build and test

```text
pio run -e esp32-c3-devkitm-1
pio test -e native_test_profile
pio test -e native_test_registry
```

## Unresolved gaps and NOT VERIFIED items

See docs/INTEGRATION_NOTES.md.

The following remain NOT VERIFIED:
- pin assignments are NOT PHYSICALLY VALIDATED;
- I2C addresses;
- GPIO15 time-share contracts;
- GPIO15 pull-up leakage;
- placeholder drivers;
- rain gauge address 0x28;
- binary size / partition headroom;
- ESP32-S3 no-OTA design.


## GATT contract

- Service: `7f2a0000-7b2a-4a6e-9a9f-1b7f7e000001`
- Descriptor request: WRITE/WRITE_NR, 3 bytes `{version, op, index}`
- Descriptor data: READ, 67 bytes `SensorDescriptorResponse`
- Sensor value: NOTIFY, 15 bytes `SensorValue`

The node starts link security immediately after connection; descriptor and request access also require encryption. NimBLE-Arduino 2.5.1 is used
with bonding + MITM + Secure Connections and `DISPLAY_ONLY` IO capability.
The six-digit passkey is generated per device, persisted in NVS, and printed on
the local serial console during provisioning/pairing.

## Build and flash

From this directory:

```text
pio run
pio run -t upload
pio device monitor -b 115200
```

The repository must provide `../shared/SensorProtocol.h` before building.
The PlatformIO environment is `esp32-c3-devkitm-1` with Arduino and NimBLE-Arduino 2.5.1.

## OTA firmware update

OTA is **ESP32-C3 sensor-node only**. The ESP32-S3 LoRa/gateway target must not receive ArduinoOTA,
Web OTA, OTA partitions, or an indirect OTA path through shared code. The ESP32-C3 partition table
remains unchanged by this feature.

The C3 uses `OtaApManager` and a local Wi-Fi AP. The AP is opened by the physical long-press
button path or an explicitly persisted AP-enabled state, and is bounded by a hard 10-minute
provisioning window. Client activity cannot extend that maximum. The open AP itself
does not grant upload authorization and the WebUI does not accept a new OTA password.

There is no station-mode OTA provisioning path and no Wi-Fi SSID/password compatibility storage.

Provision the OTA password locally over the serial console:

```text
ota password <12..64 character secret>
ota save
```

Use an isolated maintenance environment for OTA. An open AP is not a confidentiality boundary, and
production security acceptance still requires the repository's secure-boot/flash-encryption and HIL
procedures where applicable.

## Serial provisioning

At 115200 baud:

```text
help
show
name FIELD-SENSOR-01
ota password <12..64 printable ASCII characters>
ota save
save
```

`save` stores only the node name in the `sensor` NVS namespace and reboots.
The selected profile owns the sensor roster; there is no legacy example-sensor
fallback and no runtime profile detection/hot-swap path.

## Pairing

1. Flash the node and open the serial console.
2. Note the printed six-digit `BLE pairing passkey`.
3. On the gateway/commissioning client, scan for the configured node name and
   service UUID.
4. Connect and complete passkey pairing.
5. The gateway requests descriptor index 0, 1, ... and reads each 67-byte response.
6. Subscribe to the sensor-value characteristic to receive 15-byte notifications.

If the passkey is lost, erase the node NVS during development and reprovision it.
Do not expose the serial provisioning console on an unattended production device.

## Deep sleep

`DEEP_SLEEP_ENABLED` is `false` by default because a continuously connected
sensor node is the intended gateway mode. If enabled, the node advertises for
500 ms and then sleeps for five seconds when no client is connected. This mode
requires gateway-side reconnect/discovery logic and is not a transparent
substitute for a continuously connected GATT peripheral.
