#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "GnssManager.h"
#include "LoRaManager.h"
#include "AudioManager.h"
#include "StorageManager.h"
#include "WebUi.h"

GnssManager gnss;
LoRaManager lora;
AudioManager audio;
StorageManager storage;
WebServer server(Config::WEB_PORT);
WebUi web(server);

static uint32_t lastStatus = 0;
static uint32_t lastReport = 0;

static void setupWifi() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (WiFi.softAP(Config::AP_SSID, Config::AP_PASSWORD)) {
    StateLock lock(gState);
    if (lock.ok()) gState.wifiReady = true;
  }
}

static void taskGnss(void*) {
  for (;;) {
    gnss.task();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void taskLoRa(void*) {
  for (;;) {
    lora.task();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void taskAudio(void*) {
  for (;;) {
    audio.task();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static void taskWeb(void*) {
  for (;;) {
    web.task();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  delay(300);
  Serial.println("\nFieldRadio WROOM32E PCB boot");

  gState.mutex = xSemaphoreCreateMutex();
  if (!gState.mutex) {
    Serial.println("FATAL: state mutex");
    for (;;) delay(1000);
  }

  SPI.begin(Board::SPI_SCK, Board::SPI_MISO, Board::SPI_MOSI);

  bool sdOk = storage.begin();
  bool gpsOk = gnss.begin();
  bool loraOk = lora.begin();
  bool audioOk = audio.begin();

  setupWifi();
  web.begin();

  audio.btStart();

  xTaskCreatePinnedToCore(taskGnss, "GNSS", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(taskLoRa, "LoRa", 6144, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(taskAudio, "Audio", 8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(taskWeb, "Web", 6144, nullptr, 2, nullptr, 0);

  Serial.printf("SD=%d GNSS=%d LoRa=%d Audio=%d WiFi=%d\n",
                sdOk, gpsOk, loraOk, audioOk, gState.wifiReady);
}

void loop() {
  const uint32_t now = millis();

  if (now - lastStatus >= Config::STATUS_PERIOD_MS) {
    lastStatus = now;
    StateLock lock(gState);
    if (lock.ok()) {
      Serial.printf("GPS=%d %.6f,%.6f SAT=%lu LoRa=%d TX=%lu RX=%lu "
                    "REC=%d PLAY=%d BT=%d\n",
                    gState.gps.valid, gState.gps.lat, gState.gps.lon,
                    (unsigned long)gState.gps.satellites,
                    gState.loraReady,
                    (unsigned long)gState.txPackets,
                    (unsigned long)gState.rxPackets,
                    gState.recording, gState.playing, gState.btConnected);
    }
  }

  if (now - lastReport >= Config::GPS_REPORT_PERIOD_MS) {
    lastReport = now;
    lora.sendPosition();
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
