Iterasi C — Pesan 7 dari 7: main.cpp diff + platformio.ini diff

Ini pesan terakhir. Setelah ini, seluruh migrasi selesai dan Anda bisa build.

---

File 10: src/main.cpp — Perubahan

Ada 2 baris yang perlu diubah di main.cpp. Tidak ada perubahan lain.

Buka src/main.cpp, cari baris ini:

```cpp
ESPWebServerSecure server(Config::WEB_PORT);
```

Ganti menjadi:

```cpp
HttpdServer server;
```

Lalu cari baris include ini:

```cpp
#include "WebUi.h"
```

Tambahkan tepat setelah baris itu:

```cpp
#include "HttpdServer.h"
```

WebUi.h sudah include HttpdServer.h, tapi explicit include di main.cpp lebih aman untuk urutan deklarasi.

Verifikasi

Setelah dua perubahan, blok ini di main.cpp:

```cpp
GnssManager gnss;
LoRaManager lora;
LoRaWANManager lorawan(lora);
WifiStaManager wifiSta;
MqttClientManager mqtt;
CertLifecycleManager certLifecycle(mqtt);
AudioManager audio;
StorageManager storage;
SensorSpool sensorSpool;
ESPWebServerSecure server(Config::WEB_PORT);
WebUi web(server);
FuelGaugeMax17048 fuelGauge;
BleSensorReader bleSensorReader;
```

harus menjadi:

```cpp
GnssManager gnss;
LoRaManager lora;
LoRaWANManager lorawan(lora);
WifiStaManager wifiSta;
MqttClientManager mqtt;
CertLifecycleManager certLifecycle(mqtt);
AudioManager audio;
StorageManager storage;
SensorSpool sensorSpool;
HttpdServer server;
WebUi web(server);
FuelGaugeMax17048 fuelGauge;
BleSensorReader bleSensorReader;
```

Tidak ada perubahan lain di main.cpp. web.begin() di setup() tetap, web.task() di taskWeb tetap.

---

File 11: platformio.ini — Perubahan

Buka platformio.ini, cari baris lib_deps:

```ini
lib_deps =
    jgromes/RadioLib@7.7.1
    mikalhart/TinyGPSPlus@1.0.2
    knolleary/PubSubClient@2.8
    https://github.com/sh123/esp32_codec2_arduino.git#1.0.7
    adafruit/Adafruit NeoPixel@1.15.5
    h2zero/NimBLE-Arduino@2.5.1
    https://github.com/jackjansen/esp32_idf5_https_server_compat.git#master
```

Hapus baris terakhir:

```ini
    https://github.com/jackjansen/esp32_idf5_https_server_compat.git#master
```

Hasil:

```ini
lib_deps =
    jgromes/RadioLib@7.7.1
    mikalhart/TinyGPSPlus@1.0.2
    knolleary/PubSubClient@2.8
    https://github.com/sh123/esp32_codec2_arduino.git#1.0.7
    adafruit/Adafruit NeoPixel@1.15.5
    h2zero/NimBLE-Arduino@2.5.1
```

Tambahkan sdkconfig.defaults (jika belum ada)

esp_https_server adalah component opsional di ESP-IDF. PlatformIO dengan framework = arduino, espidf kadang belum mengaktifkannya. Cek apakah Anda punya file sdkconfig.defaults di root repo. Dari tree yang Anda kirim, ada sdkconfig.defaults dan sdkconfig.secure.defaults.

Buka sdkconfig.defaults. Pastikan ada baris ini:

```
CONFIG_ESP_HTTPS_SERVER_ENABLE=y
```

Kalau belum ada, tambahkan. Kalau file belum punya section apapun, isi minimal:

```
CONFIG_ESP_HTTPS_SERVER_ENABLE=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
```

Kenapa dua baris terakhir: WebUI Anda punya URI /api/storage/checksum-sha256 (30 char) dan header Origin / Cookie (cookie FR-SESSION 71 char). Default CONFIG_HTTPD_MAX_URI_LEN=512 sudah cukup, tapi CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024 mencegah header terpotong kalau ada browser yang kirim header besar (Cookie + Origin + Referer + UA bisa > 512 byte).

Verifikasi platformio.ini akhir

Setelah perubahan, [env:esp32-s3-wroom-1] harus terlihat seperti ini (baris penting saja):

```ini
[env:esp32-s3-wroom-1]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino, espidf
monitor_speed = 115200
extra_scripts =
    pre:tools/provision-web-tls.py
    pre:tools/provision-mqtt-ca.py
    pre:tools/provision-est-ca.py
upload_speed = 921600
board_build.flash_mode = qio
board_upload.flash_size = 16MB
board_upload.maximum_size = 16711680
board_build.arduino.memory_type = qio_opi
board_build.psram_type = opi
board_build.f_cpu = 240000000L
board_build.partitions = no_ota.csv

build_flags =
    -DCORE_DEBUG_LEVEL=1
    -I../shared
    -DBOARD_HAS_PSRAM
    -DFIELD_RADIO_ESP32_S3_WROOM_1_N16R8
    -DMYNEWT_VAL_BLE_MAX_CONNECTIONS=3
    -DMYNEWT_VAL_BLE_STORE_MAX_BONDS=8
    -DMYNEWT_VAL_BLE_STORE_MAX_CCCDS=8

lib_deps =
    jgromes/RadioLib@7.7.1
    mikalhart/TinyGPSPlus@1.0.2
    knolleary/PubSubClient@2.8
    https://github.com/sh123/esp32_codec2_arduino.git#1.0.7
    adafruit/Adafruit NeoPixel@1.15.5
    h2zero/NimBLE-Arduino@2.5.1
```

Tidak ada perubahan lain di platformio.ini.

---

Daftar File Lengkap Setelah Migrasi

File baru (11 file)

```
src/HttpdServer.h
src/HttpdServer.cpp
src/HttpdRequest.h
src/HttpdRequest.cpp
src/HttpdResponse.h
src/HttpdResponse.cpp
src/HttpdMultipart.h
src/HttpdMultipart.cpp
```

Plus 3 file kecil yang saya sebut di Pesan 2 tapi belum Anda konfirmasi: HttpdQuery.h, HttpdUrlEncoded.h — tidak jadi dipakai. Saya inline parser query di HttpdRequest::query() dan parser urlencoded di HttpdRequest::ensureBodyParsed(). Tidak perlu file terpisah.

File yang diubah (3 file)

```
src/WebUi.h        — diganti total
src/WebUi.cpp      — diganti total
src/main.cpp       — 2 baris berubah
platformio.ini     — 1 baris dihapus
sdkconfig.defaults — 3 baris ditambahkan (kalau belum ada)
```

File yang dihapus dari dependency

esp32_idf5_https_server_compat (tidak lagi di lib_deps). File .pio/libdeps/esp32-s3-wroom-1/esp32_idf5_https_server_compat/ akan dihapus otomatis oleh PlatformIO pada build berikutnya. Jangan hapus manual.

---

Verifikasi Akhir Sebelum Build

Buka terminal di root repo:

```bash
cd /path/to/repository

# 1. Cek tidak ada sisa referensi ESPWebServerSecure di src/
grep -rn "ESPWebServerSecure\|ESPWebServer.hpp\|esp32_idf5_https_server_compat" src/ include/ platformio.ini

# 2. Cek tidak ada sisa server_.on( dengan lambda tanpa argumen
grep -rn "server_.on(" src/

# 3. Cek tidak ada server_.send(, server_.sendHeader(, server_.arg(, server_.hasArg(, server_.header(, server_.client(), server_.contentLength(), server_.upload(), server_.method(), server_.streamFile(, server_.requestAuthentication(, server_.setServerKeyAndCert(, server_.handleClient()
grep -rn "server_\.\(send\|sendHeader\|arg\|hasArg\|header\|client\|contentLength\|upload\|method\|streamFile\|requestAuthentication\|setServerKeyAndCert\|handleClient\)" src/
```

Hasil yang diharapkan:

· Perintah 1: hanya boleh muncul di komentar atau dokumentasi, tidak di kode aktif.
· Perintah 2: hanya boleh muncul di WebUi.cpp::begin() dengan lambda [](HttpdRequest& req, HttpdResponse& res).
· Perintah 3: kosong. Kalau ada yang muncul, itu bug yang harus diperbaiki sebelum build.

---

Build

Setelah 3 verifikasi di atas bersih:

```bash
pio run -e esp32-s3-wroom-1 2>&1 | tee build.log
```

Kalau build hijau

Konfirmasi ke saya. Saya akan tulis laporan akhir (§12 prompt) dengan:

· A. Temuan utama
· B. Perubahan yang dilakukan
· C. Status verifikasi (build, test)
· D. File patch (saya akan generate .patch dari diff yang ada)
· E. Gap tersisa

Kalau build gagal

Kirim ke saya 10-20 baris pertama error dari build.log (cukup bagian error: saja, bukan seluruh log). Saya akan perbaiki dan kirim pesan tambahan.

Error yang saya antisipasi (dari analisis sebelumnya)

Error Perbaikan yang saya siapkan
'nothrow' is not a member of 'std' di HttpdServer.cpp/HttpdRequest.cpp Tambah #include <new>
designated initializers are a C99 feature di HttpdServer.cpp (baris HTTPD_SSL_CONFIG_DEFAULT()) Ganti ke inisialisasi field-per-field manual
'tskIDLE_PRIORITY' was not declared di HttpdServer.cpp Tambah #include <freertos/FreeRTOS.h> dan #include <freertos/task.h>
'SensorProtocol.h' file not found di WebUi.h Ganti ke #include "shared/SensorProtocol.h" atau tambah include path
esp_https_server.h: No such file or directory Tambah CONFIG_ESP_HTTPS_SERVER_ENABLE=y di sdkconfig.defaults, jalankan pio run -t clean lalu build ulang
undefined reference to WebUi::handleConfig Sudah saya kirim di Pesan 6d
undefined reference to WebUi::handleRoot dst Pastikan semua 121 handler tersalin; hitung dengan grep -c "void WebUi::handle" src/WebUi.cpp — harus 121
HTTP_UPLOAD_BUFLEN not declared Compat layer punya #define HTTP_UPLOAD_BUFLEN 1436 di ESPWebServer.hpp yang sudah tidak di-include. Saya sudah tidak pakai. Kalau masih muncul, cari referensinya di WebUi.cpp — seharusnya tidak ada.

---

Setelah Build Hijau: Yang Perlu Anda Test

Tanpa perangkat fisik, hanya bisa verifikasi statis. Kalau Anda punya akses perangkat:

1. Flash firmware.
2. Buka serial monitor (pio device monitor -b 115200).
3. Cari log:
   ```
   WebUI: HTTPS server ready on port 443 (121 handlers)
   HttpdServer: started on port 443 with 121/121 handlers (0 failures)
   ```
   Kalau angka bukan 121, ada handler yang gagal register — kirim log ke saya.
4. Di browser iPhone, buka https://<ip-esp32>/. Kalau TLS cert Anda valid, muncul halaman UI. Kalau self-signed, terima peringatan.
5. Login dengan user/password dari secrets/.
6. Test beberapa endpoint:
   ```
   curl -k -u <user>:<pass> https://<ip>/api/version
   curl -k -u <user>:<pass> https://<ip>/api/status
   ```
7. Test upload WAV (paling berisiko): pilih file WAV kecil, upload lewat UI.
8. Test config save: ubah satu field, klik "Save config".

Kalau ada yang gagal, kirim log serial + endpoint yang gagal.

---

Ringkasan Migrasi

Aspek Sebelum Sesudah
Server ESPWebServerSecure (compat layer di atas esp32_https_server) HttpdServer (native esp_https_server)
Dependency jackjansen/esp32_idf5_https_server_compat Tidak ada
Handler count 121 server_.on(...) 121 server_.on(...) → httpd_register_uri_handler
Request model server_.handleClient() di task httpd internal task (tidak ada handleClient)
Arg parsing compat layer auto-merge query + body HttpdRequest::arg() unified parser
Multipart HTTPMultipartBodyParser dari compat HttpdMultipart custom streaming parser
Auth server_.requestAuthentication() res.send401Basic(...)
Response server_.send() + sendHeader() res.sendXxx() + res.setHeader()
Stream file server_.streamFile() res.streamFile() (chunked)
TLS setServerKeyAndCert(DER) server_.begin(cert, len, key, len, port) → httpd_ssl_start

RAM yang dihemat:

· Tidak ada std::vector<RequestHandler> + std::function per 121 endpoint dari compat layer (~beberapa KB).
· Tidak ada HTTPRequest/HTTPResponse object per request dari compat layer (~2-4 KB per request).
· Tidak ada String argumen per request dari compat layer.
· httpd_ssl_config.httpd.max_open_sockets = 4 (compat pakai 4 juga, tapi compat default 4 di HTTPServer(port, 4) — sama).

RAM yang ditambah:

· HttpdServer::entries_[160] → sizeof(Entry) * 160 = (sizeof(httpd_uri_t) + 16) * 160. httpd_uri_t ~40 byte → total ~9 KB statis.
· std::map<String,String> bodyFields_ per request urlencoded → ~beberapa ratus byte per request.
· HttpdResponse::responded_ flag → 1 byte.
· Task httpd internal: stack 12 KB (di-set di config.httpd.stack_size).

Net: cenderung lebih hemat di steady-state, tapi initial DRAM footprint +9 KB karena tabel handler. Fragmentasi heap jangka panjang lebih rendah karena tidak ada String per request dari compat.

---

Konfirmasi

Setelah 2 perubahan di main.cpp dan 1 perubahan di platformio.ini (plus sdkconfig.defaults) selesai:

1. Jalankan 3 perintah verifikasi grep di atas.
2. Jalankan pio run -e esp32-s3-wroom-1.
3. Kirim hasil: hijau atau error pertama.

Saya tunggu hasil build. Kalau hijau, saya tulis laporan akhir §12 dan generate .patch. Kalau gagal, saya kirim fix.