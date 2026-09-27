<!-- ENH-5: Automated serial smoke checklist documentation. -->
# Hardware Checklist

## Cara pakai

Jalankan:

```sh
tools/run_hardware_checklist.sh /dev/ttyUSB0
```

Argumen adalah path serial device. Script menggunakan `stty`, `cat`, dan `timeout` tanpa `python serial.tools`.

## Otomasi vs manual

Otomasi:
1. Membaca serial selama 5 detik dan mencari `FIELDREADY`.
2. Mengirim `AT+STATUS` dan menunggu respons selama 2 detik untuk `OK`.
3. Menampilkan `CONFIG MIGRATION:` sebagai informasi `SKIP`.
4. Menampilkan `[CFG-TXN]` sebagai informasi `SKIP`.

Manual:
- Pemeriksaan kelistrikan, RF, audio, sensor, tombol, storage, dan seluruh acceptance hardware/HIL lainnya tetap harus dilakukan secara manual sesuai prosedur produksi.

Script ini BUKAN pengganti HIL acceptance.

Script keluar dengan status 0 setelah checklist berjalan; kegagalan item checklist tidak menggagalkan script. Jika serial device tidak dapat dibuka, script keluar dengan status 1 dan menampilkan `cannot open serial`.
