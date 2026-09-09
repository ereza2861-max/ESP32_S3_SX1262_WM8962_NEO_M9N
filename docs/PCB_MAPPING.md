# PCB mapping source of truth

Mapping berikut berasal dari `ESP32_WROOM32E_SX1276_WM8960_NEO_M8N_KiCad` yang diberikan.

- GPIO16/17: NEO-M8N UART
- GPIO18/19/23: shared SPI
- GPIO27: SX1276 NSS
- GPIO26: SX1276 RESET
- GPIO35: SX1276 DIO0
- GPIO36: SX1276 DIO1
- GPIO13: microSD CS
- GPIO21/22: WM8960 I2C
- GPIO32: WM8960 BCLK
- GPIO33: WM8960 LRCLK
- GPIO25: ESP32 -> WM8960 DACDAT
- GPIO34: WM8960 ADCDAT -> ESP32
- external 24 MHz oscillator: WM8960 MCLK

Tidak ada pin dedicated button/PTT/SOS atau battery ADC pada mapping yang diberikan,
sehingga kontrol PTT/SOS dipertahankan melalui web API dan dapat ditambahkan kemudian
melalui GPIO yang benar-benar dirutekan pada revisi PCB.
