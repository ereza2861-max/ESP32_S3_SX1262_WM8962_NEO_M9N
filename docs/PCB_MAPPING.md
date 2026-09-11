# PCB mapping source of truth — ESP32-S3-WROOM-1

Mapping berikut adalah target mapping firmware ESP32-S3 dan harus tetap identik dengan `include/BoardConfig.h`. Ini membutuhkan rerouting PCB dari mapping ESP32-WROOM-32E lama.

- GPIO12/13/11: shared SPI (SCK/MISO/MOSI)
- GPIO10: SX1276 NSS
- GPIO14: SX1276 RESET
- GPIO2: SX1276 DIO0
- GPIO15: SX1276 DIO1
- GPIO16: microSD CS
- GPIO8/9: WM8960 I2C
- GPIO4: WM8960 BCLK
- GPIO5: WM8960 LRCLK
- GPIO6: ESP32-S3 -> WM8960 DACDAT
- GPIO7: WM8960 ADCDAT -> ESP32-S3
- GPIO19/20: native USB D-/D+
- external 24 MHz oscillator: WM8960 MCLK

Tidak ada pin dedicated button/PTT/SOS atau battery ADC pada mapping yang diberikan,
sehingga kontrol PTT/SOS dipertahankan melalui web API dan dapat ditambahkan kemudian
melalui GPIO yang benar-benar dirutekan pada revisi PCB.
