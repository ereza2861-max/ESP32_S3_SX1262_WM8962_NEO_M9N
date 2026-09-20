# FieldRadio ESP32-C3 BLE Sensor Node

PlatformIO project for the BLE peripheral/counterpart of the gateway `SensorProtocol.h`.
The canonical wire contract is `../shared/SensorProtocol.h`; do not create a second copy.

## Hardware example

| Function | ESP32-C3 DevKitM-1 default |
|---|---:|
| BME280 SDA | GPIO8 |
| BME280 SCL | GPIO9 |
| BME280 VCC/GND | 3.3 V / GND |
| Battery divider ADC | GPIO4 |
| Button/reed input | GPIO5 to GND, internal pull-up |

The firmware uses calibrated `analogReadMilliVolts()` with 11 dB attenuation for
the battery ADC and applies `BATTERY_DIVIDER_RATIO` in software. Never expose a
voltage above the ESP32-C3 ADC/input limits. GPIO2 is excluded because it is a
strapping pin; interactive provisioning also rejects flash/USB-JTAG/strapping
GPIOs.

The example registers up to five sensors: temperature, humidity, pressure, battery voltage,
and a digital/reed input. The registry itself supports eight descriptors.

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

The ESP32-C3 sensor node owns its OTA lifecycle independently of the ESP32-S3 gateway. OTA uses the ArduinoOTA network transport with a per-device provisioned password and a dedicated dual-application partition table. The ESP32-S3 does not store or execute the C3 firmware image.

Provision the node locally before enabling OTA:

```text
wifi ssid <ssid>
wifi pass <password>
ota password <12..64 character secret>
ota save
```

The node reconnects as a Wi-Fi station at boot and exposes ArduinoOTA only after a successful connection and valid stored OTA password. A failed Wi-Fi connection does not stop sensor/BLE operation. Do not expose OTA to an untrusted network; use an isolated management network and production flash-encryption/secure-boot provisioning where required by the deployment security policy.

The OTA image is written to the inactive application slot. The bootloader selects the new slot after a successful transfer, so an interrupted transfer does not overwrite the running application. Automatic application rollback is not enabled by this patch; production validation should include boot-failure recovery before deployment.

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
