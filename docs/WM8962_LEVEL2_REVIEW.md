# WM8962 / SX1262 Level-2 Hardware-Firmware Review

## Audit scope

This review reconciles the current firmware with the supplied Rev-C package.
The supplied freelancer package contains a *schematic reference* and GPIO
contract, but no native KiCad schematic/netlist. Therefore pin-level PCB
verification below is limited to the documented contract; it is not an ERC
verification of the actual PCB.

## WM8962 register findings

1. The WM8962 device ID/reset register is R15 and reports `0x6243`; the driver
   now treats this as a presence check.
2. The 24 MHz MCLK -> 12 MHz FLL reference -> 90.3168 MHz VCO -> 11.2896 MHz
   SYSCLK configuration is valid for 44.1 kHz. The FLL parameters are:
   OUTDIV=7, REFCLK_DIV=1, FRATIO=0, N=7, THETA=329, LAMBDA=625.
3. WM8962 R7 bit 6 selects codec master mode; R7 I2S format and 16-bit width
   are explicitly configured.
4. R14 is the LRCLK divisor. `32` is used for 32 BCLKs/LRCLK, giving
   1.4112 MHz BCLK at 44.1 kHz.
5. The previous implementation wrote R31/R34 as zero. That leaves the
   PGA-to-mixer path disabled. Level-2 fixes this and makes IN1/IN2/IN3
   routing explicit.
6. Headphone/speaker analogue power-up and mixer routing are *not* declared
   production-final because the actual WM8962 schematic and external load
   network are absent from the supplied package. The PCB must be reviewed
   against the WM8962 reference design before enabling aggressive output
   power/mixer settings.

## SX1262 pin findings

For RadioLib SX126x the module constructor requires CS, DIO1, RESET and BUSY.
DIO1 is the interrupt line; BUSY is a mandatory handshake line. The current
firmware mapping is therefore:

- GPIO10 = NSS/CS
- GPIO17 = RESET
- GPIO14 = DIO1/IRQ
- GPIO15 = BUSY

Historical Rev-B documents incorrectly described the SX1262 control pins.
The current Rev-C contract above is the only mapping to route on the PCB.

DIO2 is deliberately not assigned by firmware because its electrical function
depends on the actual SX1262 RF front-end: it may control an RF switch on some
modules. The final schematic must confirm whether DIO2 is routed to an RF
switch before enabling `setDio2AsRfSwitch()`.

The RadioLib TCXO parameter is now explicit. `0.0 V` means XTAL/no TCXO control;
change this only if the final SX1262 schematic actually uses a TCXO.

## Hardware gates before fabrication

- Provide the actual KiCad schematic/netlist for U2/U3 and RF/audio sections.
- Confirm WM8962 IN1/IN2/IN3 wiring against the chosen microphone and line
  connector circuits.
- Confirm all WM8962 supplies: DCVDD, DBVDD, AVDD, CPVDD, MICVDD, PLLVDD,
  SPKVDD1 and SPKVDD2, including local decoupling.
- Confirm headphone charge-pump capacitors and DC-servo/output network.
- Confirm speaker impedance and whether the output is mono/stereo.
- Confirm SX1262 BUSY and DIO1 are physically routed to the stated GPIOs.
- Confirm SX1262 DIO2/RF-switch topology and TCXO/XTAL choice.
- Validate 50-ohm RF matching and antenna path with VNA.
- Validate 24 MHz MCLK amplitude/duty cycle at the WM8962 pin.
- Measure BCLK=1.4112 MHz and LRCLK=44.1 kHz at the codec/ESP32 pins.

## Acceptance measurements

Minimum bring-up evidence:

1. WM8962 I2C address ACK and R15 readback `0x6243`.
2. FLL lock/stable 11.2896 MHz SYSCLK.
3. BCLK 1.4112 MHz, LRCLK 44.1 kHz, I2S MSB-first waveform.
4. ADC capture with known 1 kHz input on each routed source.
5. DAC playback into the final headphone/speaker load.
6. SX1262 reset/BUSY/DIO1 timing and successful RX/TX.
7. SX1262 current and RF output measured at the intended frequency.
