# WM8962 final register-by-register bring-up audit

This audit is aligned with the Cirrus Logic WM8962 Rev 4.4 datasheet. The
firmware target is ESP32-S3-WROOM-1-N16R8 (16 MiB Quad SPI flash + 8 MiB
Octal SPI PSRAM), with 44.1 kHz / 16-bit I2S with the WM8962 as BCLK/LRCLK master,
24 MHz MCLK, IN1/IN2/IN3 as documented by `PCB_MAPPING.md`, and headphone
playback through the direct DAC-to-HPOUT bypass path.

## Register audit

| Register | Firmware role | Final state / decision |
|---|---|---|
| R5 (05h) | DAC mute | Start muted, soft mute enabled. |
| R6 (06h) | DAC unmute | Soft unmute enabled; slow ramp selected. |
| R7 (07h) | I2S format | Master, I2S, 16-bit. |
| R8 (08h) | SYSCLK/BCLK | FLL SYSCLK enabled, BCLK divider 8. |
| R14 (0Eh) | BCLK/LRCLK ratio | 32 BCLK per LRCLK. |
| R15 (0Fh) | ID/reset | Read must be `0x6243`; write `0x6243` performs software reset. |
| R25 (19h) | VMID/bias/ADC/input power | VMID normal mode `01` after startup; input/ADC blocks enabled; MICBIAS only for IN1 microphone mode. |
| R26 (1Ah) | DAC/HP PGA power and mute | HP/DAC enables are supplied by the default headphone sequencer; explicit mute uses bits 1:0. |
| R27 (1Bh) | Sample rate | `SAMPLE_RATE=000` for 44.1 kHz. |
| R28 (1Ch) | Anti-pop/VMID | Startup bias + VMID buffer enabled before analogue bring-up. |
| R31 (1Fh) | Input mixer enables | L/R mixers enabled. |
| R32/R33 (20h/21h) | Input mixer gains | Reset 0 dB values retained; PGA/direct paths are selected by R34. |
| R34 (22h) | Input source selection | `0x0009` PGA/IN1, `0x0024` IN2 direct, `0x0012` IN3 direct. |
| R37/R38 (25h/26h) | PGA source | IN1 selected and PGA enabled only for microphone mode. |
| R60 (3Ch) | Input DC servo | Enable + startup correction on L/R after every input-source change. |
| R61 (3Dh) | Headphone DC servo | Enabled and started by the default headphone power-up sequence. |
| R66 (42h) | DC-servo status | Firmware waits for both input or both headphone completion flags. |
| R63 (3Fh) | Output mixer enables | Speaker mixers remain disabled. |
| R64/R65 (64h/65h) | HP mixer routing | `0x0000` deliberately selects direct DACL/DACR bypass to HPOUTL/HPOUTR. |
| R72 (48h) | Charge pump | Enabled by the default headphone power-up sequence. |
| R90 (5Ah) | Write sequencer | `0x0080` starts the datasheet DAC-to-headphone startup sequence. |

The existing code's `VMID=0x0180` selected the fast 2x5k VMID divider permanently.
The patch uses fast startup only through the datasheet sequence and restores
the normal 2x50k setting (`VMID_SEL=01`) afterwards.

The existing code also manually powered HPOUT/DAC blocks without running the
WM8962's default output sequence. That bypassed the intended charge-pump,
DC-servo, delayed-enable, and short-removal ordering. The patch delegates this
critical ordering to the built-in sequencer.

## Final power-up order

1. Verify WM8962 R15 reads `0x6243`.
2. Software-reset the codec and wait for the reset interval.
3. Program the 24 MHz -> 12 MHz FLL reference and 11.2896 MHz SYSCLK.
4. Configure 44.1 kHz sample-rate and I2S master mode.
5. Configure input mixer/source and keep DAC muted.
6. Enable VMID/startup bias and buffered VMID.
7. Configure the direct DAC-to-headphone bypass path.
8. Start the WM8962 default DAC-to-headphone power-up sequence with R90=`0x0080`.
9. Wait for HP DC-servo completion; the sequence may take up to 93 ms.
10. Keep DAC muted until a valid I2S stream is ready.
11. Restore VMID to normal `01`.
12. For each input source, enable only the required path, run the input DC
    servo, wait for completion, then begin ADC capture.

The firmware must not enable the speaker Class-D path because the supplied PCB
package does not prove the SPKOUT load, boost supply, and Class-D external
network.

## 1 kHz acceptance test

Use an external low-distortion 1 kHz sine source. Do not use the ESP32 DAC/I2S
playback tone as the input stimulus.

Recommended initial test level:
- IN1 microphone path: 10--30 mVrms at the codec input, with the actual
  microphone bias network fitted.
- IN2/IN3 line paths: start at 100 mVrms and increase only after confirming
  clean ADC headroom.
- Source impedance: use a low source impedance (100 ohm or less) for the bench
  generator unless the PCB's intended line driver specifies otherwise. The
  WM8962 input impedance varies with PGA gain, so the PCB's actual coupling
  capacitor and source network remain the final authority.
- Use 1 uF AC-coupling per analogue input unless the final schematic specifies
  a different value. The WM8962 datasheet recommends 1 uF as a good general
  starting value.

For each of IN1, IN2 and IN3, independently:

1. Select the source in firmware.
2. Confirm the unused source paths are disconnected in R34.
3. Capture at least 4096 stereo samples at 44.1 kHz.
4. Compute RMS, peak and a 1 kHz spectral-bin magnitude.
5. Accept when the 1 kHz component is dominant, both expected channels contain
   signal, no unexpected channel is present above the project's crosstalk
   limit, and no sample is clipping.
6. Repeat after five source changes to verify the DC-servo and routing remain
   stable.
7. Repeat with the input physically disconnected to establish the noise floor.

Suggested bring-up evidence to archive per input:
`source`, generator Vrms, PGA setting, RMS L/R, peak L/R, 1 kHz magnitude L/R,
noise-floor RMS, clipping count, and DC offset.

## Hardware gates that firmware cannot prove

The repository does not contain a native KiCad netlist. Before calling this
bring-up production-final, verify on the PCB:

- AVDD, DCVDD, DBVDD, PLLVDD, MICVDD and CPVDD are within the WM8962
  recommended ranges.
- VMIDC has the required local decoupling.
- CPVOUTP/CPVOUTN and CPCA/CPCB have the recommended charge-pump capacitors.
- HPOUTFB and the required HPOUT Zobel networks are populated when headphone
  outputs are enabled.
- Each analogue input has the required DC-blocking capacitor.
- IN1/IN2/IN3 routing exactly matches `PCB_MAPPING.md`.
- The 24 MHz MCLK waveform is present at the codec.
- BCLK is approximately 1.4112 MHz and LRCLK is 44.1 kHz.
- The final headphone load is within the WM8962 headphone-driver limits.

The patch intentionally does not enable the speaker Class-D path by default.
Its compile-time contract lives in `include/Config.h`:
`CLASS_D_ENABLED`, `CLASS_D_OUTPUT_MODE`, `CLASS_D_MONO`,
`CLASS_D_SPEAKER_IMPEDANCE_OHMS`, `CLASS_D_EXPECTED_SPKVDD_MV`,
`CLASS_D_BOOST_LEVEL`, and `CLASS_D_MAX_SPKVDD_CURRENT_MA`.
The branch is compile-time (`if constexpr`) so a disabled Class-D build emits
no speaker activation path. When enabled, the firmware rejects single-ended
Class-D, accepts only 8 ohm stereo or 4 ohm mono, and requires an explicitly
declared 3.3 V or 5.0 V SPKVDD hardware contract. The current limit is an
engineering-budget guard only; the firmware cannot measure or regulate SPKVDD
current. The WM8962 datasheet defines the Class-D speaker output as BTL, with
8 ohm stereo and 4 ohm mono configurations.

Do not set `CLASS_D_ENABLED=true` until the PCB/netlist proves SPKVDD,
SPKGND, SPKOUTLP/LN/RP/RN, the speaker load, and the required output network.
