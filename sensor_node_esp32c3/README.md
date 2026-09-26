# FieldRadio ESP32-C3 BLE Sensor Node

PlatformIO project for the BLE peripheral/counterpart of the gateway `SensorProtocol.h`.
The canonical wire contract is `../shared/SensorProtocol.h`; do not create a second copy.

## Profile hardware contract

The sensor node has four mutually exclusive profiles selected at boot by a two-bit static selector.
Prototype hardware used a DIP switch; production uses a solder-jumper configuration.
Only one profile's sensor cabling is installed at a time; changing profile means physically
unplugging the previous profile's sensor cables and installing the new profile's PCB/cabling.
Pin overlap between different profiles is therefore intentional. The MFRC522 RFID reader is the
shared exception and must not be overlapped by profile-specific wiring.

Profile rosters are source-level contracts and are not physical validation evidence. Sensor models,
I2C addresses, UART/SDI-12 protocol variants, ADC calibration, and time-shared GPIO behavior still
require hardware verification where marked as placeholders.

- **Profile 0 — Island/Sea:** six sensors; GPIO3/GPIO4 are the two ADC1 channels used by the
  wave and turbidity inputs. Legacy battery monitoring is not part of this profile.
- **Profile 1 — Tropical Forest:** twelve sensors, at the current registry limit; three DS18B20
  channels share OneWire GPIO3 and the rain gauge uses the I2C bus at the locked placeholder
  address `RAIN_GAUGE_I2C_ADDRESS = 0x28`.
- **Profile 2 — Volcanic Mountain:** eight sensors. GPIO3 is time-shared between the wind-vane
  ADC and the ADXL355 chip-select; GPIO11 is used for the wind-pulse input. The integration layer
  must serialize the two GPIO3 modes and allow the ADC to settle before sampling.
- **Profile 3 — Sub-Zero Snow:** nine sensors using UART/SDI-12 and I2C substitutions where
  dedicated ADC/pulse pins are unavailable. GPIO3 is shared by the optional DS18B20 OneWire bus
  and ADXL355 CS; the estimated pull-up leakage is a **hardware-validation-required** item.

No profile implementation should fabricate a sensor reading when its hardware-specific parser or
calibration is not implemented; such drivers report stale quality instead.
## ESP32-S3 LoRa/gateway OTA design

The ESP32-S3 LoRa/gateway node has NO OTA by explicit design.

## Pin map

| Function | Pin |
|---|---:|
| Prototype DIP bit0 / production selector bit0 | GPIO0 |
| Prototype DIP bit1 / production selector bit1 | GPIO1 |
| MFRC522 CS | GPIO7 |
| MFRC522 SCK | GPIO6 |
| MFRC522 MOSI | GPIO5 |
| MFRC522 MISO | GPIO4 |
| I2C SDA | GPIO8 |
| I2C SCL | GPIO9 |
| Button / long-press | GPIO10 |
| Wind pulse | GPIO11 |
| UART1 RX | GPIO18 |
| UART1 TX | GPIO19 |
| RFID RST | GPIO20 |
| Buzzer | GPIO21 |

All pin assignments are source-level only and NOT PHYSICALLY VALIDATED.

## Sensor profiles

| Profile | Roster count | ID range | Status |
|---|---:|---|---|
| Profile 0 — Island/Sea | 6 | 0x0100..0x0105 | Source-level roster; hardware/protocol validation remains required |
| Profile 1 — Tropical Forest | 12 | 0x0200..0x020B | Exact fit at MAX_SENSORS; hardware/protocol validation remains required |
| Profile 2 — Volcanic Mountain | 8 | 0x0300..0x0307 | Source-level roster; GPIO3 time-sharing requires hardware validation |
| Profile 3 — Sub-Zero Snow | 9 | 0x0400..0x0408 | Source-level roster; GPIO3 leakage requires hardware validation |

Profile 1 is an exact 12-sensor fit at MAX_SENSORS.

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
- GPIO3 time-share contracts;
- GPIO3 pull-up leakage;
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

The C3 uses `OtaApManager` and a local Wi-Fi AP. The AP is opened only by the physical long-press
button path (or an explicitly persisted AP-enabled state), is bounded by a 10-minute provisioning
window, and requires the already-provisioned OTA password for firmware upload. The open AP itself
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
sensors 4
pin battery 4
pin digital 5
save
```

`save` stores the name, example sensor count, and pin map in NVS and reboots.
The current example driver supports 0..5 configured sensors; future drivers can
register additional descriptors without changing the BLE wire contract.

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
