# FieldRadio LoRaWAN Class A

> **Project status (2026-09-17):** No node is operating and the PCB has not been fabricated. Rev-C is a pre-fabrication design target; pin rolling remains acceptable until fabrication is explicitly recorded. See `docs/PROJECT_STATUS.md`.

FieldRadio uses the existing SX1262 as a LoRaWAN Class A end-device. It is
not a gateway or packet forwarder. The SX1262 is shared with the existing
encrypted P2P service through `RadioArbiter`.

## Regional profile

The default profile is **AS923-2**. RadioLib provides AS923, AS923-2, AS923-3
and AS923-4 regional profiles.

For Indonesia, verify the active SDPPI/Komdigi rule and the network-server
frequency plan before transmission. The Indonesian LPWAN allocation is
920-923 MHz with a 1% uplink and 1% downlink duty-cycle limit in the cited
regulatory material. AS923-2's LoRaWAN default channels are in the
921.4/921.6 MHz area; do not replace these with the P2P 923 MHz default.

## Provisioning

1. Register the device in TTN or ChirpStack as a LoRaWAN 1.0.x Class A end
   device.
2. Select the matching AS923-2 frequency plan.
3. Copy the 16-hex-character DevEUI and JoinEUI and the 32-hex-character
   AppKey into the authenticated WebUI.
4. Enable LoRaWAN, select OTAA, set FPort (default 1), and save.
5. Press **Connect** or use `lw connect` on the serial console.
6. For ABP, provide DevAddr, NwkSKey and AppSKey and select ABP.

Credentials and the RadioLib persistence buffers are stored in the `fieldradio`
NVS namespace. The firmware persists the LoRaWAN Nonces buffer after every
activation attempt (including rejected OTAA joins) and persists the Session
buffer after activation and every successful uplink. This is required so
DevNonce and frame counters are not reused after a reset. Production builds
must use ESP-IDF NVS encryption together with flash encryption. The firmware
prints a warning if `CONFIG_NVS_ENCRYPTION` is not enabled and never prints
the credential values.

## Uplink payload

The periodic telemetry payload is compact JSON:

`[latE5,lonE5,altM,sat,batteryPercent,sos,utcEpoch]`

For example:

`[-798000,11263000,100,8,95,0,1760000000]`

The array representation is intentional: it keeps normal payloads within the
51-byte DR0 application payload budget. Coordinates use degrees multiplied by
100000.

## Downlink

Downlinks received during the Class A RX1/RX2 windows are queued in RAM.
The queue contains four entries. MAC commands are processed by RadioLib; ADR
is enabled for the LoRaWAN node.

## Radio sharing

While LoRaWAN owns the radio, the P2P task is suspended. P2P acquisition uses
the `RadioArbiter`, and P2P operations are given priority when both services
request the radio. PTT/SOS activity should therefore be treated as a reason
to defer LoRaWAN use.

LoRaWAN join and confirmed uplinks can occupy the SX1262 for substantially
longer than a normal P2P packet. Do not use LoRaWAN for voice streaming.

## TTN / ChirpStack test

After joining, send a small test payload such as `hello` on FPort 1. Confirm
the uplink counter increases and inspect RX1/RX2 downlinks in the WebUI.

For TTN, the device application must use the same regional frequency plan and
activation credentials. For ChirpStack, configure the matching AS923-2
band/profile and LoRaWAN 1.0.x session parameters.

## AP versus STA trade-off

The existing Wi-Fi **AP mode remains important for an off-grid device**:
a phone/tablet can connect directly without a router and perform local
provisioning and diagnostics.

STA mode is useful when the radio has access to an infrastructure network,
but it creates a dependency on a router/AP and its credentials. In an
environment without a router, STA-only provisioning can strand the device.
The optional STA scaffold is therefore deliberately not allowed to replace
the existing AP provisioning path.

## Known limitations / TODO

- AES-GCM P2P wire version 4 is enabled for the primary P2P packet path.
- X25519/ECDH key rotation is scaffolded only.
- Wi-Fi STA and MQTT contain partial/scaffold integration and are not production-secure end-to-end; BLE provisioning remains scaffolded.
- Fragment payloads use a dedicated `LORA_TYPE_FRAG_DATA` wire type and the existing authenticated ACK path. Full 8-frame bitmap Selective Repeat remains a follow-up item.
- The exact SDPPI/Komdigi deployment frequency plan must be verified against
  the current network operator and regulatory release before field TX.
- Join is intentionally serialized with P2P radio ownership; a single SX1262
  cannot serve both protocols simultaneously.
