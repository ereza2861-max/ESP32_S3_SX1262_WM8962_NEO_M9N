# FieldRadio dependency/version audit

| Component | Source | Current | Target (pinned) | Notes |
|---|---|---|---|---|
| pioarduino platform-espressif32 | `platformio.ini` | `55.03.39` | `55.03.39` | Pinned release artifact for the IDF-5 Arduino stack. |
| ESP-IDF | pioarduino platform | resolved by platform | `5.5.4` | `55.03.39` uses Arduino-ESP32 3.3.9 / ESP-IDF 5.5.4. |
| Arduino-ESP32 core | pioarduino platform | transitive | `3.3.9` | Resolved by pioarduino `55.03.39`. |
| NimBLE-Arduino | PlatformIO Registry | `^2.5.1` | `2.5.1` | Exact version pinned in `lib_deps`. |
| PubSubClient | PlatformIO Registry | `^2.8` | `2.8` | Exact version pinned in `lib_deps`. |
| RadioLib | PlatformIO Registry | `^7.7.1` | `7.7.1` | Exact version pinned in `lib_deps`. |
| TinyGPSPlus | PlatformIO Registry | unpinned | `1.0.2` | Exact version pinned in `lib_deps`; registry metadata has a known issue, but the requested version is correct. |
| Adafruit BME280 | PlatformIO Registry | `^2.2.4` (C3) | `2.3.0` | Exact version pinned in `lib_deps`. |
| Adafruit NeoPixel | PlatformIO Registry | unpinned | `1.15.5` | Exact version pinned in `lib_deps`. |
| ESPWebServerSecure | Git URL | `jackjansen/esp32_idf5_https_server_compat#master` | **NOT VERIFIED** | The dependency is aligned with the IDF-5 build baseline, but this audit environment cannot resolve/build the remote dependency; an immutable commit pin should be recorded once the validated dependency revision is known. |
| esp32-camera | PlatformIO/IDF | not present | `N/A` | No dependency found in current repository. |
| mbedtls | ESP-IDF component | transitive | `2.28.x` | Bundled by ESP-IDF v5.5.4; cannot be independently pinned. |
| esp-audio-simple-dec | ESP-IDF component | not present | `N/A` | No dependency found in current manifest. |
| esp-sr (AEC) | ESP-IDF component | `^2.5.2` | `2.5.2` | Exact version pinned in `idf_component.yml`. |
| usb_device_uac | ESP-IDF component | `1.3.1` | `1.3.1` | Exact version already pinned. |
| zlib | ESP-IDF bundled component | transitive | `ESP-IDF v5.5.4 bundled` | Bundled by ESP-IDF v5.5.4; cannot be independently pinned. |

ESP-IDF v5.5.4 and Arduino-ESP32 core v3.3.9 are resolved by pioarduino `55.03.39` rather than independently declared component versions.

| MramStorage driver | native (repo) | — | inline | Everspin MR25H256 SPI, 40 MHz, Mode 0. No external dependency. |
| Everspin MR25H256 | hardware | — | device | 256 Kbit SPI MRAM, SOIC-8, CS = GPIO42. |
