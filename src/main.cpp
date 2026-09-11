#include <Arduino.h>
#include <cstring>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "GnssManager.h"
#include "LoRaManager.h"
#include "AudioManager.h"
#include "StorageManager.h"
#include "WebUi.h"
#include "PersistentConfig.h"

GnssManager gnss;
LoRaManager lora;
AudioManager audio;
StorageManager storage;
WebServer server(Config::WEB_PORT);
WebUi web(server);

static uint32_t lastStatus = 0;
static uint32_t lastReport = 0;
static uint32_t lastSos = 0;
static uint32_t lastBatterySample = 0;
static uint32_t criticalBatterySince = 0;

static void watchdogInit() {
  esp_task_wdt_config_t cfg{};
  cfg.timeout_ms = Config::TASK_WDT_TIMEOUT_MS;
  cfg.idle_core_mask = (1U << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1U;
  cfg.trigger_panic = true;

  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&cfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("WARN: Task WDT setup failed: %s\n", esp_err_to_name(err));
  }
}

static void watchdogSubscribe() {
  const esp_err_t err = esp_task_wdt_add(nullptr);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("WARN: Task WDT subscribe failed: %s\n", esp_err_to_name(err));
  }
}

static void updateBattery(uint32_t now) {
#if defined(ARDUINO_ARCH_ESP32)
  if (Board::BATTERY_ADC < 0 ||
      (lastBatterySample != 0 &&
       now - lastBatterySample < Config::BATTERY_SAMPLE_PERIOD_MS)) {
    return;
  }
  lastBatterySample = now;

  const uint32_t mv = analogReadMilliVolts(Board::BATTERY_ADC);
  const float voltage =
      (static_cast<float>(mv) / 1000.0f) * Config::BATTERY_DIVIDER_RATIO *
      gConfig.batteryCalibration;

  StateLock lock(gState);
  if (!lock.ok()) return;
  gState.batteryAvailable = mv > 0;
  gState.batteryV = gState.batteryAvailable ? voltage : NAN;
  gState.batteryLow = gState.batteryAvailable &&
                      voltage <= Config::BATTERY_LOW_THRESHOLD;
  gState.batteryCritical = gState.batteryAvailable &&
                           voltage <= Config::BATTERY_CRITICAL;
#endif
}

static bool shouldDeepSleep(uint32_t now) {
  bool critical = false;
  bool busy = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    critical = gState.batteryCritical;
    busy = gState.ptt || gState.sos || gState.recording ||
           gState.playing || gState.usbAudioActive;
  }

  if (critical) {
    if (!criticalBatterySince) criticalBatterySince = now;
    if (now - criticalBatterySince >= Config::CRITICAL_SHUTDOWN_DELAY_MS)
      return true;
  } else {
    criticalBatterySince = 0;
  }

  static uint32_t idleSince = 0;
  if (!Config::DEEP_SLEEP_ENABLED || busy) {
    idleSince = now;
    return false;
  }

  if (!idleSince) idleSince = now;
  if (now - idleSince >= Config::DEEP_SLEEP_IDLE_MS) {
    return true;
  }
  return false;
}

static void enterDeepSleep() {
  Serial.println("POWER: entering deep sleep");
  Serial.flush();
  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}


static bool credentialsConfigured() {
  return gConfig.apPassword.length() >= 8 &&
         gConfig.webUser.length() > 0 &&
         gConfig.webPassword.length() >= 8 &&
         gConfig.apPassword != gConfig.webPassword;
}

static void setupWifi() {
  if (!credentialsConfigured()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "Set unique WiFi/web credentials";
    Serial.println("WARN: WiFi AP disabled until unique credentials are configured");
    return;
  }

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (WiFi.softAP(gConfig.apSsid.c_str(), gConfig.apPassword.c_str())) {
    StateLock lock(gState);
    if (lock.ok()) gState.wifiReady = true;
  }
}

static void taskGnss(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    gnss.task();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void taskLoRa(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    lora.task();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void taskAudio(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    audio.task();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static void taskWeb(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    web.task();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  delay(300);
  Serial.println("\nFieldRadio ESP32-S3-WROOM-1 boot");
  gConfig.load();
  watchdogInit();

  gState.mutex = xSemaphoreCreateMutex();
  gSpiMutex = xSemaphoreCreateMutex();
  if (!gState.mutex || !gSpiMutex) {
    Serial.println("FATAL: mutex initialization");
    for (;;) delay(1000);
  }

  SPI.begin(Board::SPI_SCK, Board::SPI_MISO, Board::SPI_MOSI);

#if defined(ARDUINO_ARCH_ESP32)
  if (Board::BATTERY_ADC >= 0) {
    pinMode(Board::BATTERY_ADC, INPUT);
    analogSetPinAttenuation(Board::BATTERY_ADC, ADC_11db);
  }
#endif

  bool sdOk = storage.begin();
  bool gpsOk = gnss.begin();
  bool loraOk = lora.begin();
  bool audioOk = audio.begin();
  audio.setVolume(gConfig.volume);

  setupWifi();
  web.begin();

  const bool usbAudioOk = audio.usbStart();

  bool tasksOk = true;
  tasksOk &= (xTaskCreatePinnedToCore(taskGnss, "GNSS", 4096, nullptr, 3, nullptr, 1) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskLoRa, "LoRa", 12288, nullptr, 4, nullptr, 1) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskAudio, "Audio", 8192, nullptr, 5, nullptr, 0) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskWeb, "Web", 6144, nullptr, 2, nullptr, 0) == pdPASS);

  if (!tasksOk) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "FreeRTOS task creation failed";
    Serial.println("WARN: one or more FreeRTOS tasks failed to start");
  }

  bool wifiOk = false;
  {
    StateLock lock(gState);
    if (lock.ok()) wifiOk = gState.wifiReady;
  }
  Serial.printf("SD=%d GNSS=%d LoRa=%d Audio=%d USB=%d WiFi=%d\n",
                sdOk, gpsOk, loraOk, audioOk, usbAudioOk, wifiOk);
}

void loop() {
  static bool wdtSubscribed = false;
  if (!wdtSubscribed) {
    watchdogSubscribe();
    wdtSubscribed = true;
  }
  esp_task_wdt_reset();
  const uint32_t now = millis();
  updateBattery(now);

  if (now - lastStatus >= Config::STATUS_PERIOD_MS) {
    lastStatus = now;
    StateLock lock(gState);
    if (lock.ok()) {
      Serial.printf("GPS=%d %.6f,%.6f SAT=%lu LoRa=%d TX=%lu RX=%lu "
                    "REC=%d PLAY=%d USB=%d BAT=%.2fV LOW=%d CRIT=%d\n",
                    gState.gps.valid, gState.gps.lat, gState.gps.lon,
                    (unsigned long)gState.gps.satellites,
                    gState.loraReady,
                    (unsigned long)gState.txPackets,
                    (unsigned long)gState.rxPackets,
                    gState.recording, gState.playing, gState.usbAudioActive,
                    gState.batteryV, gState.batteryLow, gState.batteryCritical);
    }
  }

  if (now - lastReport >= Config::GPS_REPORT_PERIOD_MS) {
    lastReport = now;
    lora.sendPosition();
  }

  bool sosActive = false;
  {
    StateLock lock(gState);
    if (lock.ok()) sosActive = gState.sos;
  }
  if (sosActive && now - lastSos >= Config::SOS_REPEAT_MS) {
    lastSos = now;
    if (!lora.sendSOS()) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "SOS repeat TX failed";
    }
  }

  if (shouldDeepSleep(now)) {
    enterDeepSleep();
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
