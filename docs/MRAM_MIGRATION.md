# FRAM → MRAM migration

Date: 2026-09-30

Baseline: `c5ca35829a09dcc191ff2d75e1be3fa1934172c4`

## Hardware

- Everspin MR25H256, 256 Kbit, SOIC-8
- SPI Mode 0, 40 MHz
- SCK GPIO12, MISO GPIO13, MOSI GPIO11
- CS GPIO42
- GPIO42 is exclusively MRAM CS; the obsolete TX-indicator assignment is removed.
- RGB TX/RX indication remains on GPIO39.
- USB-Serial-JTAG remains GPIO19/GPIO20.

## MRAM memory map

| Address | Purpose |
|---|---|
| `0x0000-0x000F` | ReplayStore header |
| `0x0010-0x040F` | ReplayStore bank 0, 32 × 32-byte slots |
| `0x0410-0x080F` | ReplayStore bank 1, 32 × 32-byte slots |
| `0x0810-0x0FFF` | Reserved |
| `0x1000-0x1FFF` | PersistentConfig slot A |
| `0x2000-0x2FFF` | PersistentConfig slot B |
| `0x3000` | PersistentConfig commit A (`0xA5`) |
| `0x3001` | PersistentConfig commit B (`0xA5`) |
| `0x3010-0x301F` | Migration marker (`MRM1`) |
| `0x3020-0x7FFF` | Free / future use |

All addresses are centralized in `include/Config.h`.

## ReplayStore

ReplayStore uses MRAM as its authoritative backend after a valid MRAM header is detected.
Each logical slot has two physical banks. Records carry a monotonic generation and CRC32.
Generation zero remains readable using the legacy entry-only CRC format. Writes target the
inactive bank, so a failed/torn target write does not replace the previous valid generation.
NVS journal fallback is permitted only before MRAM becomes authoritative; there is no silent
downgrade after authority has been established.

## PersistentConfig

PersistentConfig uses two 4-KB MRAM slots. A full record is written to the inactive slot,
read back and validated, and only then is the corresponding `0xA5` commit byte written.
The migration marker is written only after the committed record has been verified. The newest
valid committed generation is authoritative.

## NVS migration and credential security

Existing NVS configuration remains the migration source and pre-authority fallback. NVS is
not deleted. Credential-bearing fields remain in encrypted NVS; they are explicitly stripped
from the MRAM configuration payload. This includes LoRaWAN keys, Wi-Fi passwords, web password
salt/hash, and EST credentials/tokens.

Once a valid `MRM1` marker exists, runtime configuration loads from MRAM and does not silently
fall back to NVS if the MRAM record is invalid.

## Schema history

The migration is primarily a physical backend migration. Existing logical configuration
schema history is retained; schema version changes are not introduced merely because storage
moved from FRAM/NVS to MRAM.

## Hardware validation checklist

- [ ] MR25H256 responds correctly
- [ ] SPI Mode 0 at 40 MHz
- [ ] GPIO42 CS
- [ ] read/write/verify
- [ ] shared-bus coexistence with microSD
- [ ] shared-bus coexistence with SX1262
- [ ] reboot during ReplayStore commit
- [ ] reboot during PersistentConfig commit
- [ ] reboot during NVS → MRAM migration
- [ ] MRAM-authoritative boot
- [ ] pre-authority NVS fallback
- [ ] no silent downgrade after `MRM1` marker

Hardware validation was not run in the available build environment.
