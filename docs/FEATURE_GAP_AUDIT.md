# Rev-C feature-gap audit and implementation gates

> **Gap-closure addendum (target `b5b87b27`, 2026-10-01):** The canonical
> behavioral contract is now `docs/BEHAVIORAL_CONTRACT.md`. The dedicated RX
> LED is not a board symbol; RGB is the TX/RX indicator. Battery ADC health
> bookkeeping is scheduled even when MAX17048 is present. Sensor sampling uses
> descriptor `periodMs`, while BLE notification remains a separate 1-second
> cadence. `LORA_TYPE_SENSOR_TELEMETRY` now has an application consumer and
> carries the existing spool `sampleId` in its pre-existing padding. Sensor OTA
> AP access is protected and `/profile` requires authenticated headers.

> **Project status (2026-09-17):** No node is operating and the PCB has not been fabricated. Rev-C is a pre-fabrication candidate; pin rolling remains acceptable until fabrication is explicitly recorded. See `docs/PROJECT_STATUS.md`.

The firmware target is ESP32-S3-WROOM-1-N16R8 (16 MiB Quad SPI flash + 8 MiB Octal SPI PSRAM).

This patch implements only features that can be made deterministic from the supplied
firmware and the GPIO contract. It deliberately does **not** claim to implement every
item in the requested backlog.

## GPIO decision

The requested GPIO39..42 block plus GPIO47/48 is electrically usable as general GPIO on ESP32-S3.
GPIO0/3/45/46 are strapping pins and GPIO33..37 may be consumed by flash/PSRAM, so
neither group is used for the new outputs. GPIO39..42 also carry JTAG functions; they are usable as normal GPIO after boot, but
they cannot simultaneously serve as an active JTAG debug connection. GPIO46 is deliberately
not used for an LED because it is a strapping pin.

The firmware allocation is:

| Function | GPIO | Notes |
|---|---:|---|
| Buzzer | 47 | active-high; passive buzzer needs PWM/transistor stage |
| Addressable RGB | 39 | one-wire data; requires actual addressable LED |
| Haptic | 40 | active-high driver enable; do not drive a motor directly from GPIO |
| Charging LED | 41 | charge-probable heuristic only; no charger STAT input is defined |
| TX/RX status | 39 | addressable RGB; priority contract in `docs/BEHAVIORAL_CONTRACT.md` |
| RX LED | — | Dedicated RX LED removed; RX is already represented by the addressable RGB status LED so GPIO48 can be reserved for I2C SCL. |
| PTT | 21 | active-high RTC wake input; external pulldown required |
| GNSS 1-PPS | 9 | dedicated digital timing input; no longer an analog spare |
| SOS | 18 | active-high RTC wake input; external pulldown required |
| Battery ADC | 1 | ADC1 routed input |
| MAX2016 forward | 2 | ADC1 |
| MAX2016 reflected | 8 | ADC1 |

GPIO43/44 are used for GNSS despite being the default UART0 TX/RX pins; this is a deliberate GPIO-Matrix/UART routing choice.

## Implemented in this patch

- compile-time GPIO collision detection;
- TX indicator plus RGB-based RX status; no dedicated RX LED net;
- addressable RGB status indication;
- haptic/buzzer feedback for physical PTT/SOS;
- battery sample/cycle counters persisted in encrypted NVS when NVS encryption is provisioned;
- explicit battery health telemetry in `/api/status`;
- one-time migration away from the legacy plaintext WebUI password NVS key;
- salted iterative password hash for WebUI credential storage;
- constant-time password verification;
- configuration version bump and automatic migration/save;
- configuration audit log with rotation;
- cppcheck CI baseline;
- GNSS 1-PPS input on GPIO9 with periodic 12-hour GNSS time synchronization;
- MAX17048 VCELL/SOC polling with bounded I2C access and ADC fallback.

## Important limitations

### Battery health
Cycle count is a voltage-threshold estimate. Capacity fade cannot be measured correctly
without coulomb counting/current measurement or a fuel-gauge IC. The charging LED is
therefore only a "charge probable" indication based on a positive voltage slope.

### Secure Boot / flash encryption
The repository already contains a production-only `sdkconfig.secure.defaults`. Enabling
Secure Boot V2 or flash encryption automatically in the normal build would be unsafe
without the real signing key, eFuse provisioning procedure, and recovery/update policy.
Do not turn these on merely because a compile-time placeholder exists.

### HTTPS
The WebUI now uses the IDF5-compatible HTTPS server on port 443 and refuses to fall back
to plaintext HTTP when certificate material is missing. Device-specific certificate and
private-key DER files are provisioned locally and validated before the firmware embeds
them. A self-signed certificate is suitable for lab use; production deployments should
use a controlled CA/device-certificate process and the Secure Boot/flash-encryption
profile described in `docs/SECURITY_PROVISIONING.md`.

### LoRa fragmentation, selective-repeat/SACK, voice ACK, store-and-forward, mesh routing
The fragment transport is now bounded and implemented inside the existing authenticated
packet envelope: maximum 2048 bytes, maximum 16 fragments, TX window 8, receiver SACK
bitmap 8 bits, and independent per-fragment retransmission. Fragment ACKs use the existing
`LORA_TYPE_TEXT_ACK` message type with an 8-byte SACK payload; ordinary 6-byte text ACKs
remain unchanged. This is a greenfield protocol decision: no legacy node interoperability
is required because no node is operating and the PCB has not been fabricated.

The remaining production gate is RF/HIL validation under loss, reordering, duplicate,
power-loss and SD-recovery conditions. Store-and-forward remains SD-backed and bounded;
mesh routing continues to use the authenticated routing extension and TTL/replay controls.

### GNSS dead reckoning / AGPS
These require additional sensors or an offline assistance-data source. GPS-only firmware
cannot truthfully implement inertial dead reckoning.

### Opus/CELP, filesystem journaling, cloud backup, WebSocket/PWA
These are architectural changes with RAM/flash/storage/network consequences and should be
implemented only after selecting the codec/library, filesystem, transport and update
strategy. They are not silently represented as "implemented" by this patch.

### Factory test, HIL, fuzzing and unit tests
The CI static-analysis hook is added, but hardware tests still need a fixture and a
native-test harness. No fake pass result is generated.


## QnA D-06 closure

D-06 is CLOSED with option C: full PKI certificate lifecycle using RFC 7030 EST.
EST authentication is selected by D-06: mode 0 uses the factory client certificate;
mode 1 uses Basic Auth; mode 2 uses a bootstrap Bearer token for first enrollment.
Mode 2 removes the bootstrap token from encrypted NVS after successful enrollment.
Renewal generates a new P-256 key pair and CSR, validates the issued certificate,
and commits the replacement through NVS certificate/key slots with a commit marker.
`/api/mqtt/cert-status`, `/api/mqtt/cert-renew`, `/api/mqtt/cert-history`, and
`/api/mqtt/cert-cacerts` expose the authenticated lifecycle operations.

The firmware expects an actual RFC 7030 EST endpoint or backend adapter. It does
not assume that an MQTT broker URL is itself an EST endpoint. Automatic lifecycle
is disabled by default and is guarded by valid GNSS UTC time.

## D-06 implementation gap closure notes

The previously identified gaps are closed by the PKI lifecycle completion patch:
custom EST CA provisioning and trust-anchor use, EST auth modes 1/2, encrypted-NVS
credential persistence, bootstrap-token cleanup, rotated certificate history,
bounded PEM encoding, mock-server isolation/authentication, configuration
concurrency coverage, and the merged HIL power-loss procedure.


## Audit response disposition — 2026-10-02

Closed in source: GAP-I-01, GAP-I-02, GAP-I-03, GAP-R-03, GAP-S-01,
GAP-X-01, GAP-X-02, GAP-SEC-03, GAP-D-01, GAP-D-02, GAP-D-03 and the
locked Q-A01/Q-A02/Q-B01/Q-B02/Q-C01/Q-C02/Q-C03/Q-D01/Q-E01/Q-E02/Q-F01
decisions.

Deferred as evidence-only or hardware-dependent: physical PCB validation,
RF/HIL interoperability, Secure Boot/Flash Encryption/eFuse manufacturing
evidence, brownout/power-loss hardware evidence, and electrical validation of
the GPIO3 mux and sensor fixtures. These cannot be proven by a source patch
without inventing hardware evidence.
