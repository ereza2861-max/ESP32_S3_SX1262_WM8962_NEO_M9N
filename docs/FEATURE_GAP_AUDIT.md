# Rev-B feature-gap audit and implementation gates

The firmware target is ESP32-S3-WROOM-1-N16R8 (16 MiB Quad SPI flash + 8 MiB Octal SPI PSRAM).

This patch implements only features that can be made deterministic from the supplied
firmware and the GPIO contract. It deliberately does **not** claim to implement every
item in the requested backlog.

## GPIO decision

The requested GPIO38..42 block is electrically usable as general GPIO on ESP32-S3.
GPIO0/3/45/46 are strapping pins and GPIO33..37 may be consumed by flash/PSRAM, so
neither group is used for the new outputs. GPIO39..42 also carry JTAG functions; they
are safe as normal GPIO after boot but cannot simultaneously be used for an active
JTAG debug connection.

The firmware allocation is:

| Function | GPIO | Notes |
|---|---:|---|
| Buzzer | 38 | active-high assumption; passive buzzer needs PWM/transistor stage |
| Addressable RGB | 39 | one-wire data; requires actual addressable LED |
| Haptic | 40 | driver enable; do not drive a motor directly from GPIO |
| Charging LED | 41 | only a charge-probable heuristic because no charger STAT input is defined |
| TX LED | 42 | dedicated |
| RX LED | 48 | dedicated |
| PTT | 21 | existing active-low input |
| SOS | 47 | existing active-low input |
| Battery ADC | 1 | existing divider input |

GPIO43/44 are intentionally not used because they are the default UART0 TX/RX pins.

## Implemented in this patch

- compile-time GPIO collision detection;
- independent TX/RX indicators;
- addressable RGB status indication;
- haptic/buzzer feedback for physical PTT/SOS;
- battery sample/cycle counters persisted in encrypted NVS when NVS encryption is provisioned;
- explicit battery health telemetry in `/api/status`;
- one-time migration away from the legacy plaintext WebUI password NVS key;
- salted iterative password hash for WebUI credential storage;
- constant-time password verification;
- configuration version bump and automatic migration/save;
- configuration audit log with rotation;
- cppcheck CI baseline.

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
The current WebServer is plaintext HTTP. Password hashing protects credentials at rest
but does not protect HTTP Basic credentials on the network. HTTPS requires a TLS-capable
server plus certificate/private-key provisioning. It is intentionally not fabricated here.

### LoRa fragmentation, voice ACK, store-and-forward, mesh routing
These require a protocol-versioned packet format and persistent queue semantics. The
existing protocol already has authenticated envelopes, TTL, deduplication, text/SOS ACK,
LBT and hop support. A production fragmentation/voice-ACK extension should be added as
a new protocol version rather than changing the meaning of existing packets.

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
