#include <Arduino.h>
#include <cstring>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/task.h>
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
static bool lastPttButton = false;
static bool lastSosButton = false;
static bool rawPttButton = false;
static bool rawSosButton = false;
static uint32_t pttDebounceMs = 0;
static uint32_t sosDebounceMs = 0;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static uint32_t wifiIdleSince = 0;
static uint32_t wifiRetryMs = 0;
static volatile uint32_t hbGnss = 0, hbLoRa = 0, hbAudio = 0, hbWeb = 0;
static TaskHandle_t hGnss = nullptr, hLoRa = nullptr, hAudio = nullptr, hWeb = nullptr;

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
  if (gState.batteryAvailable) {
    const float pct = (voltage - Config::BATTERY_PERCENT_EMPTY_V) *
                      100.0f /
                      (Config::BATTERY_PERCENT_FULL_V -
                       Config::BATTERY_PERCENT_EMPTY_V);
    gState.batteryPercent = static_cast<int8_t>(constrain(pct, 0.0f, 100.0f));
  } else {
    gState.batteryPercent = -1;
  }
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

  // SX1262 DIO1 is the configured IRQ/wake line. Keep the radio in RX and
  // allow a high level on DIO1 to wake the S3. The radio is reinitialized
  // normally after wake, so no volatile application state is required here.
  esp_sleep_enable_ext1_wakeup(1ULL << Board::LORA_DIO1, ESP_EXT1_WAKEUP_ANY_HIGH);

  (void)audio.stopRecording();
  audio.stopPlayback();
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.ptt = false;
      gState.recording = false;
      gState.playing = false;
    }
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(Config::DEEP_SLEEP_WAKE_GRACE_MS);
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
    ++hbGnss;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void taskLoRa(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    lora.task();
    ++hbLoRa;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void taskAudio(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    audio.task();
    ++hbAudio;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static void taskWeb(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    web.task();
    ++hbWeb;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}


static void handlePhysicalControls(uint32_t now) {
  const bool pttRaw = Board::BTN_PTT >= 0 && digitalRead(Board::BTN_PTT) == LOW;
  const bool sosRaw = Board::BTN_SOS >= 0 && digitalRead(Board::BTN_SOS) == LOW;

  if (pttRaw != rawPttButton) { rawPttButton = pttRaw; pttDebounceMs = now; }
  if (sosRaw != rawSosButton) { rawSosButton = sosRaw; sosDebounceMs = now; }

  const bool pttPressed = (now - pttDebounceMs >= BUTTON_DEBOUNCE_MS) ? rawPttButton : lastPttButton;
  const bool sosPressed = (now - sosDebounceMs >= BUTTON_DEBOUNCE_MS) ? rawSosButton : lastSosButton;

  if (pttPressed != lastPttButton) {
    lastPttButton = pttPressed;
    if (pttPressed) {
      (void)audio.playTone(1000, 60);
      if (audio.startRecording()) {
        StateLock lock(gState);
        if (lock.ok()) gState.ptt = true;
      }
    } else {
      (void)audio.stopRecording();
      StateLock lock(gState);
      if (lock.ok()) gState.ptt = false;
      (void)audio.playTone(700, 60);
    }
  }

  if (sosPressed && !lastSosButton) {
    lastSosButton = true;
    if (lora.sendSOS()) {
      StateLock lock(gState);
      if (lock.ok()) gState.sos = true;
    }
    (void)audio.playTone(1400, 150);
  } else if (!sosPressed) {
    lastSosButton = false;
  }

  bool tx = false, rx = false, rec = false;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      tx = gState.ptt;
      rx = gState.rxActive;
      rec = gState.recording;
      if (gState.rxActive && now - gState.rxActivityMs > Config::RX_ACTIVITY_HOLD_MS)
        gState.rxActive = false;
    }
  }
  if (Board::STATUS_LED >= 0) digitalWrite(Board::STATUS_LED, (tx || rx || rec) ? HIGH : LOW);
}

static void manageWifi(uint32_t now) {
  if (WiFi.getMode() != WIFI_AP) return;
  if (WiFi.softAPgetStationNum() > 0) {
    wifiIdleSince = now;
    return;
  }
  if (!wifiIdleSince) wifiIdleSince = now;
  if (now - wifiIdleSince >= Config::WIFI_AP_IDLE_TIMEOUT_MS) {
    // softAPdisconnect() can leave the Wi-Fi mode set to WIFI_AP. Switch the
    // radio fully off so the retry branch below can actually restart the AP.
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    StateLock lock(gState);
    if (lock.ok()) gState.wifiReady = false;
    wifiRetryMs = now;
    wifiIdleSince = now;
    return;
  }
  if (WiFi.getMode() == WIFI_OFF && now - wifiRetryMs >= Config::WIFI_AP_RETRY_MS) {
    wifiRetryMs = now;
    setupWifi();
  }
}


static void taskHealth(void*) {
  watchdogSubscribe();
  uint32_t last[4] = {0, 0, 0, 0};
  uint32_t lastCheck = millis();
  for (;;) {
    esp_task_wdt_reset();
    const uint32_t now = millis();
    if (now - lastCheck >= 5000) {
      const uint32_t hb[4] = {hbGnss, hbLoRa, hbAudio, hbWeb};
      bool stalled = false;
      uint8_t stalledMask = 0;
      for (size_t i = 0; i < 4; ++i) {
        if (hb[i] == last[i]) {
          stalled = true;
          stalledMask |= static_cast<uint8_t>(1U << i);
        }
        last[i] = hb[i];
      }
      StateLock lock(gState);
      if (lock.ok()) {
        gState.heapFree = ESP.getFreeHeap();
        gState.gnssStackMin = hGnss ? uxTaskGetStackHighWaterMark(hGnss) : 0;
        gState.loraStackMin = hLoRa ? uxTaskGetStackHighWaterMark(hLoRa) : 0;
        gState.audioStackMin = hAudio ? uxTaskGetStackHighWaterMark(hAudio) : 0;
        gState.webStackMin = hWeb ? uxTaskGetStackHighWaterMark(hWeb) : 0;
        if (stalled) {
          ++gState.healthAlerts;
          HealthLogEntry& e = gState.healthLog[gState.healthLogNext];
          e.timestamp = gState.gps.timeValid ? gState.gps.utcEpoch : now;
          e.stalledMask = stalledMask;
          e.heapFree = gState.heapFree;
          e.gnssStackMin = gState.gnssStackMin;
          e.loraStackMin = gState.loraStackMin;
          e.audioStackMin = gState.audioStackMin;
          e.webStackMin = gState.webStackMin;
          gState.healthLogNext =
              (gState.healthLogNext + 1) % Config::HEALTH_LOG_SIZE;
          if (gState.healthLogCount < Config::HEALTH_LOG_SIZE)
            ++gState.healthLogCount;
        }
      }
      lastCheck = now;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
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
  if (Board::BTN_PTT >= 0) pinMode(Board::BTN_PTT, INPUT_PULLUP);
  if (Board::BTN_SOS >= 0) pinMode(Board::BTN_SOS, INPUT_PULLUP);
  if (Board::STATUS_LED >= 0) pinMode(Board::STATUS_LED, OUTPUT);
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
  tasksOk &= (xTaskCreatePinnedToCore(taskGnss, "GNSS", 4096, nullptr, 3, &hGnss, 1) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskLoRa, "LoRa", 12288, nullptr, 4, &hLoRa, 1) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskAudio, "Audio", 8192, nullptr, 5, &hAudio, 0) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskWeb, "Web", 6144, nullptr, 2, &hWeb, 0) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskHealth, "Health", 4096, nullptr, 1, nullptr, 0) == pdPASS);

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
  handlePhysicalControls(now);
  manageWifi(now);

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
  bool sosEscalated = false;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      sosActive = gState.sos;
      sosEscalated = gState.sosEscalated;
    }
  }
  // SOS retries are owned by LoRaManager. Avoid creating a new sequence here,
  // which would otherwise reset the ACK/retry state every few seconds.
  if ((sosActive || sosEscalated) && now - lastSos >= Config::SOS_BEACON_PERIOD_MS) {
    if (lora.sendPosition()) {
      StateLock lock(gState);
      if (lock.ok()) {
        ++gState.sosBeaconCount;
        gState.sosLastBeaconMs = now;
      }
    }
    lastSos = now;
  }

  if (shouldDeepSleep(now)) {
    enterDeepSleep();
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
