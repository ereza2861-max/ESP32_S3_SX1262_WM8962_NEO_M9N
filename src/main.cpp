#include <Arduino.h>
#include <cstring>
#include <cmath>
#include <WiFi.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <Preferences.h>
#include <Wire.h>
#include "FuelGaugeMax17048.h"
#include <SD.h>
#include <nvs_flash.h>
#include <esp32-hal-cpu.h>
#include <freertos/task.h>
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include "GnssManager.h"
#include "LoRaManager.h"
#include "LoRaWANManager.h"
#include "WifiStaManager.h"
#include "MqttClientManager.h"
#include "CertLifecycleManager.h"
#include "AudioManager.h"
#include "StorageManager.h"
#include "SensorSpool.h"
#include "WebUi.h"
#include "PersistentConfig.h"
#include "BleSensorReader.h"
#include "SensorTelemetry.h"
#include <Adafruit_NeoPixel.h>

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
static uint32_t sosPressedSinceMs = 0;
static bool sosLongPressCancelled = false;
static uint32_t sosCancelCount_ = 0;
static uint32_t sosSendCount_ = 0;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t SOS_CANCEL_LONG_PRESS_MS = 1500;
static uint32_t wifiIdleSince = 0;
static uint32_t wifiRetryMs = 0;
static uint32_t wifiStaFailureSince = 0;
static bool wifiApFallbackActive = false;
static volatile uint32_t hbGnss = 0, hbLoRa = 0, hbAudio = 0, hbWeb = 0, hbLoRaWAN = 0, hbBleSensor = 0, hbSensorForward = 0;
static TaskHandle_t hGnss = nullptr, hLoRa = nullptr, hAudio = nullptr, hWeb = nullptr, hLoRaWAN = nullptr, hNet = nullptr, hBleSensor = nullptr, hSensorForward = nullptr;
static uint32_t bootCount = 0;
static Adafruit_NeoPixel rgb(1, Board::LED_RGB, NEO_GRB + NEO_KHZ800);
static uint32_t lastBatteryHealthPersist = 0;
static float previousBatteryV = NAN;
static bool batteryWasFull = false;
static bool batteryWasLow = false;
static bool sosBuzzerActive = false;
static bool sosBuzzerOn = false;
static uint8_t sosBuzzerSymbol = 0;
static uint32_t sosBuzzerDeadline = 0;
static uint16_t lastBuzzerSosSeq = 0;
static uint32_t lastLogPersistMs = 0;
static size_t lastPersistedLoraLogCount = 0;
static size_t lastPersistedHealthLogCount = 0;

static void pulseAuxiliary(uint16_t ms) {
  if (Board::HAPTIC >= 0) digitalWrite(Board::HAPTIC, HIGH);
  if (Board::BUZZER >= 0) digitalWrite(Board::BUZZER, HIGH);
  const uint32_t started = millis();
  while (millis() - started < ms) delay(1);
  if (Board::HAPTIC >= 0) digitalWrite(Board::HAPTIC, LOW);
  if (Board::BUZZER >= 0) digitalWrite(Board::BUZZER, LOW);
}

static void updateAuxiliaryIndicators(bool tx, bool rx) {
  if (Board::LED_TX >= 0) digitalWrite(Board::LED_TX, tx ? HIGH : LOW);
  if (Board::LED_RX >= 0) digitalWrite(Board::LED_RX, rx ? HIGH : LOW);
  StateLock lock(gState);
  if (lock.ok()) {
    const bool critical = gState.batteryCritical;
    const bool low = gState.batteryLow;
    rgb.setPixelColor(0, critical ? rgb.Color(255, 0, 0) :
                                 (low ? rgb.Color(255, 96, 0) :
                                  (tx ? rgb.Color(0, 0, 255) :
                                   (rx ? rgb.Color(0, 255, 0) : 0))));
    rgb.show();
    // GPIO41 cannot prove charger state without a charger STAT input. The
    // LED is therefore used only for a clearly documented "charge probable"
    // heuristic based on a sustained positive battery-voltage slope.
    if (Board::LED_CHARGING >= 0)
      digitalWrite(Board::LED_CHARGING, gState.batteryChargeProbable ? HIGH : LOW);
  }
}

static void loadBatteryHealth() {
  Preferences prefs;
  if (!prefs.begin("fieldradio", true)) return;
  const uint32_t version = prefs.getUInt("bhealth_v", 0);
  if (version == Config::BATTERY_HEALTH_VERSION) {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.batteryCycleCount = prefs.getUInt("bcycles", 0);
      gState.batterySampleCount = prefs.getUInt("bsamples", 0);
      gState.batteryMinV = prefs.getFloat("bmin", NAN);
      gState.batteryMaxV = prefs.getFloat("bmax", NAN);
    }
  }
  prefs.end();
}

static void persistBatteryHealth(uint32_t now) {
  if (now - lastBatteryHealthPersist < Config::BATTERY_HEALTH_PERSIST_MS) return;
  float minV = NAN, maxV = NAN;
  uint32_t cycles = 0, samples = 0;
  {
    StateLock lock(gState);
    if (!lock.ok() || !gState.batteryAvailable) return;
    cycles = gState.batteryCycleCount;
    samples = gState.batterySampleCount;
    minV = gState.batteryMinV;
    maxV = gState.batteryMaxV;
  }
  Preferences prefs;
  if (prefs.begin("fieldradio", false)) {
    (void)prefs.putUInt("bhealth_v", Config::BATTERY_HEALTH_VERSION);
    (void)prefs.putUInt("bcycles", cycles);
    (void)prefs.putUInt("bsamples", samples);
    (void)prefs.putFloat("bmin", minV);
    (void)prefs.putFloat("bmax", maxV);
    prefs.end();
    lastBatteryHealthPersist = now;
  }
}

static const char* wakeupCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1";
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO";
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "TOUCH";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP";
    case ESP_SLEEP_WAKEUP_UART: return "UART";
    default: return "POWERON/OTHER";
  }
}

static void enforceProductionSecurity() {
#if defined(FIELDRADIO_PRODUCTION_BUILD) || (defined(CONFIG_SECURE_BOOT_V2_ENABLED) && defined(CONFIG_SECURE_FLASH_ENC_ENABLED) && CONFIG_SECURE_BOOT_V2_ENABLED && CONFIG_SECURE_FLASH_ENC_ENABLED)
#ifndef CONFIG_SECURE_BOOT_V2_ENABLED
#define CONFIG_SECURE_BOOT_V2_ENABLED 0
#endif
#ifndef CONFIG_SECURE_FLASH_ENC_ENABLED
#define CONFIG_SECURE_FLASH_ENC_ENABLED 0
#endif
#if !CONFIG_SECURE_BOOT_V2_ENABLED || !CONFIG_SECURE_FLASH_ENC_ENABLED
  Serial.println("FATAL: production build requires Secure Boot V2 + Flash Encryption");
  for (;;) delay(1000);
#endif
#endif
}

static void recordBrownoutMarker() {
  bool brownout = false;
  {
    StateLock lock(gState);
    if (lock.ok()) brownout = gState.brownoutReset;
  }
  if (!brownout || !storage.ready()) return;
  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  File f = SD.open("/LOG/BROWNOUT.LOG", FILE_APPEND);
  if (!f) return;
  f.printf("%lu,brownout,tx_power_cap=%d,duration_ms=60000\n",
           static_cast<unsigned long>(millis()),
           static_cast<int>(Config::BATTERY_TX_POWER_LOW_DBM));
  f.close();
}

static void recordBootDiagnostics() {
  const esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  const esp_reset_reason_t reset = esp_reset_reason();
  Preferences prefs;
  if (prefs.begin("fieldradio", false)) {
    bootCount = prefs.getUInt("bootcnt", 0) + 1U;
    (void)prefs.putUInt("bootcnt", bootCount);
    prefs.end();
  }
  const bool brownout = reset == ESP_RST_BROWNOUT;
  StateLock lock(gState);
  if (lock.ok()) {
    gState.bootCount = bootCount;
    gState.wakeupCause = static_cast<uint32_t>(wake);
    gState.resetReason = static_cast<uint32_t>(reset);
    gState.brownoutReset = brownout;
    if (brownout) gState.lastError = "Brownout reset detected";
  }
  Serial.printf("BOOT: count=%lu reset=%d wake=%d(%s)%s\n",
                static_cast<unsigned long>(bootCount), static_cast<int>(reset),
                static_cast<int>(wake), wakeupCauseName(wake),
                brownout ? " BROWNOUT" : "");
}

static void setPowerProfile(bool active) {
#if CONFIG_IDF_TARGET_ESP32S3
  const uint32_t target = active ? Config::CPU_ACTIVE_MHZ : Config::CPU_IDLE_MHZ;
  static uint32_t applied = 0;
  if (applied != target && setCpuFrequencyMhz(target)) applied = target;
#endif
}

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


static uint16_t sosBuzzerDuration(uint8_t symbol) {
  return symbol < 3 || symbol >= 6 ? 180U : 540U;
}

static void serviceSosBuzzer(uint32_t now) {
  uint16_t seq = 0;
  bool sos = false;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      seq = gState.sosSeq;
      sos = gState.sos;
    }
  }
  if (!sosBuzzerActive && sos && seq != 0 && seq != lastBuzzerSosSeq) {
    lastBuzzerSosSeq = seq;
    sosBuzzerActive = true;
    sosBuzzerOn = true;
    sosBuzzerSymbol = 0;
    sosBuzzerDeadline = now + sosBuzzerDuration(0);
    if (Board::BUZZER >= 0) digitalWrite(Board::BUZZER, HIGH);
    return;
  }
  if (!sosBuzzerActive || static_cast<int32_t>(now - sosBuzzerDeadline) < 0) return;

  if (sosBuzzerOn) {
    sosBuzzerOn = false;
    if (Board::BUZZER >= 0) digitalWrite(Board::BUZZER, LOW);
    sosBuzzerDeadline = now + (sosBuzzerSymbol == 8 ? 1000U : 180U);
    return;
  }
  if (sosBuzzerSymbol == 8) {
    sosBuzzerActive = false;
    return;
  }
  ++sosBuzzerSymbol;
  sosBuzzerOn = true;
  sosBuzzerDeadline = now + sosBuzzerDuration(sosBuzzerSymbol);
  if (Board::BUZZER >= 0) digitalWrite(Board::BUZZER, HIGH);
}

static bool eraseStorageTreeBoot(const char* path) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return SD.remove(path);
  }
  bool ok = true;
  for (File child = dir.openNextFile(); child; child = dir.openNextFile()) {
    const String name = child.name();
    const bool isDir = child.isDirectory();
    child.close();
    if (isDir) ok = eraseStorageTreeBoot(name.c_str()) && ok;
    else ok = SD.remove(name) && ok;
  }
  dir.close();
  return SD.rmdir(path) && ok;
}

static bool detectEmergencyWipeAtBoot() {
#if defined(ARDUINO_ARCH_ESP32)
  if (Board::BTN_SOS < 0 || Board::BTN_PTT < 0) return false;
  pinMode(Board::BTN_SOS, INPUT_PULLDOWN);
  pinMode(Board::BTN_PTT, INPUT_PULLDOWN);
  if (digitalRead(Board::BTN_SOS) != HIGH || digitalRead(Board::BTN_PTT) != HIGH)
    return false;
  const uint32_t started = millis();
  while (millis() - started < 10000U) {
    if (digitalRead(Board::BTN_SOS) != HIGH || digitalRead(Board::BTN_PTT) != HIGH)
      return false;
    delay(20);
  }
  return true;
#else
  return false;
#endif
}

static void executeEmergencyWipe() {
  (void)nvs_flash_erase();
  SPI.begin(Board::SPI_SCK, Board::SPI_MISO, Board::SPI_MOSI);
  if (SD.begin(Board::SD_CS, SPI, 20000000U)) {
    (void)eraseStorageTreeBoot("/REC");
    (void)eraseStorageTreeBoot("/LOG");
    (void)eraseStorageTreeBoot("/TRACK");
  }
  if (Board::BUZZER >= 0) {
    for (uint8_t i = 0; i < 3; ++i) {
      digitalWrite(Board::BUZZER, HIGH);
      delay(600);
      digitalWrite(Board::BUZZER, LOW);
      delay(250);
    }
  }
}

static void persistRuntimeLogs(uint32_t now) {
  if (!storage.ready() || now - lastLogPersistMs < Config::LOG_PERSIST_PERIOD_MS) return;
  lastLogPersistMs = now;

  LoraPacketLogEntry le{};
  HealthLogEntry he{};
  size_t loraNext = 0, healthNext = 0;
  bool haveLora = false, haveHealth = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return;
    haveLora = gState.loraPacketLogCount > 0 &&
               gState.loraPacketLogNext != lastPersistedLoraLogCount;
    haveHealth = gState.healthLogCount > 0 &&
                 gState.healthLogNext != lastPersistedHealthLogCount;
    if (haveLora) {
      const size_t k = (gState.loraPacketLogNext + Config::LORA_PACKET_LOG_SIZE - 1) %
                       Config::LORA_PACKET_LOG_SIZE;
      le = gState.loraPacketLog[k];
      loraNext = gState.loraPacketLogNext;
    }
    if (haveHealth) {
      const size_t k = (gState.healthLogNext + Config::HEALTH_LOG_SIZE - 1) %
                       Config::HEALTH_LOG_SIZE;
      he = gState.healthLog[k];
      healthNext = gState.healthLogNext;
    }
  }

  bool loraSaved = !haveLora;
  bool healthSaved = !haveHealth;
  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  if (haveLora) {
    const char* const loraPath = "/LOG/LORA.LOG";
    const char* const loraOldPath = "/LOG/LORA.1.LOG";
    File f = SD.open(loraPath, FILE_APPEND);
    if (f && f.size() >= Config::LORA_LOG_ROTATE_BYTES) {
      f.close();
      if (SD.exists(loraOldPath)) SD.remove(loraOldPath);
      if (SD.exists(loraPath)) SD.rename(loraPath, loraOldPath);
      f = SD.open(loraPath, FILE_APPEND);
    }
    if (f) {
      if (f.size() == 0) f.println("epoch,tx,type,seq,source,rssi,snr,ttl");
      loraSaved = f.printf("%llu,%d,%u,%u,%lu,%d,%.1f,%u\n",
                           static_cast<unsigned long long>(le.timestamp), le.tx,
                           le.type, le.seq, static_cast<unsigned long>(le.sourceId),
                           le.rssi, le.snr, le.ttl) > 0;
      f.close();
    }
  }
  if (haveHealth) {
    const char* const healthPath = "/LOG/HEALTH.LOG";
    const char* const healthOldPath = "/LOG/HEALTH.1.LOG";
    File f = SD.open(healthPath, FILE_APPEND);
    if (f && f.size() >= Config::HEALTH_LOG_ROTATE_BYTES) {
      f.close();
      if (SD.exists(healthOldPath)) SD.remove(healthOldPath);
      if (SD.exists(healthPath)) SD.rename(healthPath, healthOldPath);
      f = SD.open(healthPath, FILE_APPEND);
    }
    if (f) {
      if (f.size() == 0) f.println("epoch,stalled,heap,gnssStack,loraStack,audioStack,webStack,largest,boot,wakeup,reset,brownout,jamming,noise,occupancy");
      healthSaved = f.printf("%llu,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d,%d,%d,%u\n",
                             static_cast<unsigned long long>(he.timestamp), he.stalledMask,
                             (unsigned long)he.heapFree, (unsigned long)he.gnssStackMin,
                             (unsigned long)he.loraStackMin, (unsigned long)he.audioStackMin,
                             (unsigned long)he.webStackMin, (unsigned long)he.heapLargestFree,
                             (unsigned long)he.bootCount, (unsigned long)he.wakeupCause,
                             (unsigned long)he.resetReason, he.brownoutReset,
                             he.jammingDetected, he.noiseFloorDbm, he.channelOccupancy) > 0;
      f.close();
    }
  }
  if (loraSaved) lastPersistedLoraLogCount = loraNext;
  if (healthSaved) lastPersistedHealthLogCount = healthNext;
}

static void watchdogSubscribe() {
  const esp_err_t err = esp_task_wdt_add(nullptr);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("WARN: Task WDT subscribe failed: %s\n", esp_err_to_name(err));
  }
}

static void updateBattery(uint32_t now) {
#if defined(ARDUINO_ARCH_ESP32)
  RuntimeConfig config;
  if (!configSnapshot(config)) return;
  if (Board::BATTERY_ADC < 0 ||
      (lastBatterySample != 0 &&
       now - lastBatterySample < Config::BATTERY_SAMPLE_PERIOD_MS)) {
    return;
  }
  lastBatterySample = now;

  const uint32_t mv = analogReadMilliVolts(Board::BATTERY_ADC);
  const float rawVoltage =
      (static_cast<float>(mv) / 1000.0f) * Config::BATTERY_DIVIDER_RATIO;
  const float voltage = rawVoltage * config.batteryCalibration;

  StateLock lock(gState);
  if (!lock.ok()) return;
  gState.batteryAvailable = mv > 0;
  gState.batteryCalibrationDrift =
      gState.batteryAvailable &&
      voltage >= Config::BATTERY_RECHARGE_START_V &&
      isfinite(rawVoltage) &&
      fabsf(Config::BATTERY_FULL_V / max(rawVoltage, 0.01f) -
            config.batteryCalibration) > 0.05f;
  gState.batteryV = gState.batteryAvailable ? voltage : NAN;
  gState.batteryLow = gState.batteryAvailable &&
                      voltage <= config.batteryLowThreshold;
  gState.batteryCritical = gState.batteryAvailable &&
                           voltage <= config.batteryCriticalThreshold;
  if (gState.batteryAvailable) {
    const float pct = (voltage - Config::BATTERY_PERCENT_EMPTY_V) *
                      100.0f /
                      (Config::BATTERY_PERCENT_FULL_V -
                       Config::BATTERY_PERCENT_EMPTY_V);
    gState.batteryPercent = static_cast<int8_t>(constrain(pct, 0.0f, 100.0f));
    const size_t bh = gState.batteryHistoryNext;
    gState.batteryHistoryMs[bh] = now;
    gState.batteryHistoryV[bh] = voltage;
    gState.batteryHistoryPercent[bh] = gState.batteryPercent;
    gState.batteryHistoryNext = (bh + 1) % RuntimeState::BATTERY_HISTORY_SIZE;
    if (gState.batteryHistoryCount < RuntimeState::BATTERY_HISTORY_SIZE)
      ++gState.batteryHistoryCount;
    if (gState.batteryHistoryCount >= 2) {
      const size_t last = (gState.batteryHistoryNext +
                           RuntimeState::BATTERY_HISTORY_SIZE - 1) %
                          RuntimeState::BATTERY_HISTORY_SIZE;
      const size_t first = (gState.batteryHistoryNext +
                            RuntimeState::BATTERY_HISTORY_SIZE -
                            min<size_t>(gState.batteryHistoryCount, 24)) %
                           RuntimeState::BATTERY_HISTORY_SIZE;
      const uint32_t dt = gState.batteryHistoryMs[last] - gState.batteryHistoryMs[first];
      const float dv = gState.batteryHistoryV[last] - gState.batteryHistoryV[first];
      if (dt >= 60000U && dv < -0.001f) {
        const float rateVPerMin = (-dv) / (static_cast<float>(dt) / 60000.0f);
        const float remainingV = max(0.0f, voltage - config.batteryCriticalThreshold);
        gState.batteryEstimatedMinutes = static_cast<int32_t>(
            constrain(remainingV / rateVPerMin, 0.0f, 100000.0f));
      } else {
        gState.batteryEstimatedMinutes = -1;
      }
    }
    ++gState.batterySampleCount;
    if (!isfinite(gState.batteryMinV) || voltage < gState.batteryMinV) gState.batteryMinV = voltage;
    if (!isfinite(gState.batteryMaxV) || voltage > gState.batteryMaxV) gState.batteryMaxV = voltage;
    const bool fullNow = voltage >= Config::BATTERY_FULL_V;
    const bool lowNow = voltage <= Config::BATTERY_CYCLE_RESET_V;
    if (fullNow) batteryWasFull = true;
    if (batteryWasFull && lowNow && !batteryWasLow) {
      ++gState.batteryCycleCount;
      batteryWasFull = false;
    }
    batteryWasLow = lowNow;
    gState.batteryChargeProbable =
        isfinite(previousBatteryV) &&
        voltage >= Config::BATTERY_RECHARGE_START_V &&
        voltage > previousBatteryV + 0.003f;
    previousBatteryV = voltage;
  } else {
    gState.batteryPercent = -1;
    gState.batteryChargeProbable = false;
  }
#endif
  persistBatteryHealth(now);
}

static bool shouldDeepSleep(uint32_t now) {
  RuntimeConfig config;
  if (!configSnapshot(config)) return false;
  bool critical = false;
  bool busy = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return false;
    critical = gState.batteryCritical;
    busy = gState.ptt || gState.sos || gState.recording ||
           gState.playing || gState.usbAudioActive;
  }
  // A continuously enabled BLE sensor gateway must not enter automatic deep
  // sleep or it would silently stop collecting external sensor nodes.
  const bool sensorKeepAwake = config.sensorReaderEnabled &&
                               config.sensorKeepAwake;
  busy = busy || sensorKeepAwake;

  if (critical) {
    if (!criticalBatterySince) criticalBatterySince = now;
    if (now - criticalBatterySince >= config.criticalShutdownDelayMs)
      return true;
  } else {
    criticalBatterySince = 0;
  }

  static uint32_t idleSince = 0;
  setPowerProfile(busy || critical);
  if (!config.deepSleepEnabled || busy) {
    idleSince = now;
    return false;
  }

  if (!idleSince) idleSince = now;
  if (now - idleSince >= config.deepSleepIdleMs) {
    return true;
  }
  return false;
}

static void enterDeepSleep() {
  RuntimeConfig config;
  if (!configSnapshot(config)) {
    Serial.println("POWER: configuration unavailable; aborting deep sleep");
    return;
  }
  Serial.println("POWER: entering deep sleep");
  Serial.flush();

  // ESP32-S3 deep-sleep wake requires RTC-capable GPIOs (GPIO0..21).
  // Use EXT1/ANY_HIGH so the SX1262 DIO1 IRQ and both active-high physical
  // buttons can share the same wake domain. The buttons require external
  // pulldowns because RTC internal pulls are not a substitute for the PCB
  // bias network while RTC power is reduced.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  uint64_t wakeMask = 0;
  if (Board::LORA_DIO1 >= 0) wakeMask |= 1ULL << Board::LORA_DIO1;
  if (Board::BTN_PTT >= 0) wakeMask |= 1ULL << Board::BTN_PTT;
  if (Board::BTN_SOS >= 0) wakeMask |= 1ULL << Board::BTN_SOS;

  const uint64_t wakePeriodUs =
      static_cast<uint64_t>(config.wakePeriodSec) * 1000000ULL;
  const esp_err_t timerWakeErr = esp_sleep_enable_timer_wakeup(wakePeriodUs);
  if (timerWakeErr != ESP_OK)
    Serial.printf("POWER: failed to configure GNSS time-sync wake (%lus): %s\n",
                  static_cast<unsigned long>(config.wakePeriodSec),
                  esp_err_to_name(timerWakeErr));

  if (wakeMask != 0) {
    const esp_err_t wakeErr =
        esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (wakeErr != ESP_OK)
      Serial.printf("POWER: failed to configure EXT1 wake: %d\n", wakeErr);
  }

  if (!lora.prepareForDeepSleep()) {
    Serial.println("POWER: LoRa duty-cycle RX arm failed; aborting deep sleep");
    return;
  }
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
  delay(config.deepSleepWakeGraceMs);
  esp_deep_sleep_start();
}


/* Test/diagnostic entry point; WebUi authentication/CSRF protects the route. */
void fieldRadioRequestDeepSleep() {
  enterDeepSleep();
}


static bool credentialsConfigured(const RuntimeConfig& config) {
  // Keep AP and web credentials independent. The AP password is plaintext
  // configuration for the access point; the web password is represented by
  // its salted hash after provisioning and must never be cross-verified.
  const bool apCredentialsConfigured_ = config.apPassword.length() >= 8;
  const bool webCredentialsConfigured_ =
      config.webUser.length() > 0 && config.webPasswordConfigured();
  return apCredentialsConfigured_ && webCredentialsConfigured_;
}

static void setupWifi() {
  RuntimeConfig config;
  if (!configSnapshot(config)) return;
  if (!credentialsConfigured(config)) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "Set unique WiFi/web credentials";
    Serial.println("WARN: WiFi AP disabled until unique credentials are configured");
    return;
  }

  WiFi.setSleep(false);
  wifiStaFailureSince = 0;
  wifiApFallbackActive = false;
  if (Config::STA_SSID[0] != 0) {
    // D-GAP-2: try persistent STA first; AP is recovery-only after the
    // configured fallback delay, rather than being exposed on every boot.
    (void)wifiSta.connect(Config::STA_SSID, Config::STA_PASSWORD);
    StateLock lock(gState);
    if (lock.ok()) gState.wifiReady = false;
    return;
  }

  // No STA credential exists, so there is nothing useful to retry. Expose
  // the configured AP immediately as the initial provisioning/recovery path.
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(config.apSsid.c_str(), config.apPassword.c_str())) {
    StateLock lock(gState);
    if (lock.ok()) gState.wifiReady = true;
  }
}

static void taskGnss(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    { StateLock lock(gState); if (lock.ok()) ++gState.wdtResetCounts[0]; }
    gnss.task();
    ++hbGnss;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void taskLoRa(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    { StateLock lock(gState); if (lock.ok()) ++gState.wdtResetCounts[1]; }
    lora.task();
    ++hbLoRa;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void taskLoRaWAN(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    { StateLock lock(gState); if (lock.ok()) ++gState.wdtResetCounts[4]; }
    lorawan.task();
    ++hbLoRaWAN;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void taskNet(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    wifiSta.task();
    certLifecycle.task();
    mqtt.task();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void taskAudio(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    { StateLock lock(gState); if (lock.ok()) ++gState.wdtResetCounts[2]; }
    audio.task();
    ++hbAudio;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static void taskWeb(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    { StateLock lock(gState); if (lock.ok()) ++gState.wdtResetCounts[3]; }
    web.task();
    ++hbWeb;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void taskBleSensor(void*) {
  watchdogSubscribe();
  for (;;) {
    esp_task_wdt_reset();
    ++hbBleSensor;
    bleSensorReader.task();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void taskSensorForward(void*) {
  watchdogSubscribe();
  struct ReportState {
    uint32_t nodeId = 0;
    uint16_t sensorId = 0;
    float value = NAN;
    uint32_t lastReportMs = 0;
    bool valid = false;
  } states[Config::SENSOR_MAX_NODES_VALUE * Config::SENSOR_MAX_SENSORS_PER_NODE_VALUE]{};

  for (;;) {
    esp_task_wdt_reset();
    ++hbSensorForward;

    static uint32_t spoolRetryMs = 0;
    if (!sensorSpool.ready() && static_cast<int32_t>(millis() - spoolRetryMs) >= 0) {
      spoolRetryMs = millis() + 5000U;
      if (storage.begin()) (void)sensorSpool.begin();
    }

    {
      StateLock lock(gState);
      if (lock.ok()) {
        gState.sensorDropped = bleSensorReader.sensorReader().droppedSamples();
        gState.sensorSpoolDepth = static_cast<uint32_t>(sensorSpool.depth());
        gState.sensorSpoolEvictions = sensorSpool.evictions();
        gState.sensorSpoolDrops = sensorSpool.drops();
        gState.sensorSpoolRecovered = sensorSpool.recovered();
        gState.peerMacFailures = bleSensorReader.sensorReader().peerMacFailures();
      }
    }

    // First make the RAM queue durable. Peek is intentional: a sample is not
    // consumed from RAM until its persistent spool record exists.
    SensorReader::SensorSample queued{};
    if (bleSensorReader.sensorReader().peekSensorForLoRa(queued)) {
      ReportState* state = nullptr;
      ReportState* freeState = nullptr;
      for (auto& candidate : states) {
        if (candidate.valid && candidate.nodeId == queued.nodeId &&
            candidate.sensorId == queued.sensorId) {
          state = &candidate;
          break;
        }
        if (!candidate.valid && !freeState) freeState = &candidate;
      }
      if (!state) state = freeState;

      uint8_t required = SensorSpool::DELIVERY_MQTT;
      if (state && SensorTelemetry::shouldReportSensor(
              state->valid ? state->value : NAN, queued.value,
              state->valid ? state->lastReportMs : 0, millis(),
              Config::SENSOR_REPORT_DELTA_THRESHOLD,
              Config::SENSOR_REPORT_PERIOD_MS)) {
        required |= SensorSpool::DELIVERY_LORA;
      }

      if (sensorSpool.append(queued, required)) {
        SensorReader::SensorSample consumed{};
        (void)bleSensorReader.sensorReader().popSensorForLoRa(consumed, 0);
      }
    }

    // Drain the persistent spool independently of the RAM queue. A downstream
    // outage therefore leaves only the undelivered bit set and does not lose
    // samples already accepted by the BLE reader.
    SensorSpool::Pending pending{};
    if (sensorSpool.peek(pending)) {
      if ((pending.requiredMask & SensorSpool::DELIVERY_MQTT) &&
          !(pending.deliveredMask & SensorSpool::DELIVERY_MQTT)) {
        if (mqtt.publishSensorData(pending.sample.nodeId, pending.sample.nodeName,
                                   pending.sample.sensorId, pending.sample.sensorName,
                                   pending.sample.unit, pending.sample.value,
                                   pending.sample.quality, pending.sample.rssi,
                                   pending.sample.timestampMs)) {
          (void)sensorSpool.markDelivered(pending.sampleId, SensorSpool::DELIVERY_MQTT);
        }
      }

      if ((pending.requiredMask & SensorSpool::DELIVERY_LORA) &&
          !(pending.deliveredMask & SensorSpool::DELIVERY_LORA)) {
        bool reportDue = true;
        ReportState* state = nullptr;
        ReportState* freeState = nullptr;
        for (auto& candidate : states) {
          if (candidate.valid && candidate.nodeId == pending.sample.nodeId &&
              candidate.sensorId == pending.sample.sensorId) {
            state = &candidate;
            break;
          }
          if (!candidate.valid && !freeState) freeState = &candidate;
        }
        if (!state) state = freeState;
        if (state) {
          reportDue = SensorTelemetry::shouldReportSensor(
              state->valid ? state->value : NAN, pending.sample.value,
              state->valid ? state->lastReportMs : 0, millis(),
              Config::SENSOR_REPORT_DELTA_THRESHOLD,
              Config::SENSOR_REPORT_PERIOD_MS);
        }
        if (reportDue &&
            lora.sendSensorTelemetry(pending.sample.nodeId, pending.sample.sensorId,
                                     pending.sample.value, pending.sample.quality,
                                     pending.sample.timestampMs)) {
          (void)sensorSpool.markDelivered(pending.sampleId, SensorSpool::DELIVERY_LORA);
          if (state) {
            state->nodeId = pending.sample.nodeId;
            state->sensorId = pending.sample.sensorId;
            state->value = pending.sample.value;
            state->lastReportMs = millis();
            state->valid = true;
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void handlePhysicalControls(uint32_t now) {
  static uint32_t modeChordSince = 0;
  static bool modeChordHandled = false;
  const bool pttRaw = Board::BTN_PTT >= 0 && digitalRead(Board::BTN_PTT) == HIGH;
  const bool sosRaw = Board::BTN_SOS >= 0 && digitalRead(Board::BTN_SOS) == HIGH;

  if (pttRaw != rawPttButton) { rawPttButton = pttRaw; pttDebounceMs = now; }
  if (sosRaw != rawSosButton) { rawSosButton = sosRaw; sosDebounceMs = now; }

  const bool pttPressed = (now - pttDebounceMs >= BUTTON_DEBOUNCE_MS) ? rawPttButton : lastPttButton;
  const bool sosPressed = (now - sosDebounceMs >= BUTTON_DEBOUNCE_MS) ? rawSosButton : lastSosButton;

  // Both physical buttons held for 1.5 s toggle the LoRaWAN opt-in mode.
  // This chord is checked before PTT/SOS handling so it cannot accidentally
  // transmit an SOS while selecting the radio mode.
  if (pttPressed && sosPressed) {
    if (!modeChordSince) modeChordSince = now;
    if (!modeChordHandled && now - modeChordSince >= 1500U) {
      modeChordHandled = true;
      RuntimeConfig candidate{};
      if (!configSnapshot(candidate)) {
        Serial.println("LORAWAN: physical mode toggle failed to snapshot config");
      } else {
        candidate.lorawanEnabled = !candidate.lorawanEnabled;
        if (!configCommit(candidate)) {
          Serial.println("LORAWAN: physical mode toggle failed to save");
        } else if (candidate.lorawanEnabled) {
          Serial.println("LORAWAN: physical mode ON");
          if (candidate.lorawanMode == 0) (void)lorawan.connectOTAA();
          else (void)lorawan.connectABP();
        } else {
          Serial.println("LORAWAN: physical mode OFF");
          (void)lorawan.disconnect();
        }
      }
    }
    lastPttButton = false;
    lastSosButton = false;
    return;
  }
  if (!pttPressed && !sosPressed) {
    modeChordSince = 0;
    modeChordHandled = false;
  }

  if (pttPressed != lastPttButton) {
    lastPttButton = pttPressed;
    if (pttPressed) {
      pulseAuxiliary(30);
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
    sosPressedSinceMs = now;
    sosLongPressCancelled = false;
    if (lora.sendSOS()) {
      ++sosSendCount_;
      StateLock lock(gState);
      if (lock.ok()) gState.sos = true;
    }
    pulseAuxiliary(80);
    (void)audio.playTone(1400, 150);
  } else if (sosPressed && lastSosButton &&
             !sosLongPressCancelled && sosPressedSinceMs != 0 &&
             now - sosPressedSinceMs >= SOS_CANCEL_LONG_PRESS_MS) {
    if (lora.cancelSOS()) {
      ++sosCancelCount_;
      sosLongPressCancelled = true;
      pulseAuxiliary(120);
      (void)audio.playTone(700, 120);
    }
  } else if (!sosPressed) {
    lastSosButton = false;
    sosPressedSinceMs = 0;
    sosLongPressCancelled = false;
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
  updateAuxiliaryIndicators(tx, rx);
}

static void manageWifi(uint32_t now) {
  RuntimeConfig config;
  if (!configSnapshot(config)) return;
  const wifi_mode_t mode = WiFi.getMode();

  if (Config::STA_SSID[0] != 0 && mode != WIFI_AP && mode != WIFI_AP_STA) {
    if (now - wifiRetryMs >= Config::WIFI_AP_RETRY_MS) {
      wifiRetryMs = now;
      setupWifi();
    }
    return;
  }

  if (Config::STA_SSID[0] != 0 && mode == WIFI_AP_STA) {
    if (wifiSta.isConnected()) {
      wifiStaFailureSince = 0;
      return;
    }
    if (wifiApFallbackActive) return;
    if (!wifiStaFailureSince) wifiStaFailureSince = now;
    if (now - wifiStaFailureSince < Config::WIFI_AP_FALLBACK_DELAY_MS) return;

    // D-GAP-2: hybrid fallback. Keep STA reconnect enabled, but expose the
    // recovery AP only after the bounded failure window has elapsed.
    if (WiFi.softAP(config.apSsid.c_str(), config.apPassword.c_str())) {
      StateLock lock(gState);
      if (lock.ok()) gState.wifiReady = true;
      wifiIdleSince = now;
      wifiApFallbackActive = true;
    }
    return;
  }

  if (mode != WIFI_AP) return;
  if (WiFi.softAPgetStationNum() > 0) {
    wifiIdleSince = now;
    return;
  }
  if (!wifiIdleSince) wifiIdleSince = now;
  if (now - wifiIdleSince >= Config::WIFI_AP_IDLE_TIMEOUT_MS) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    StateLock lock(gState);
    if (lock.ok()) gState.wifiReady = false;
    wifiRetryMs = now;
    wifiIdleSince = now;
    wifiStaFailureSince = 0;
    wifiApFallbackActive = false;
  }
}


static void taskHealth(void*) {
  watchdogSubscribe();
  uint32_t last[7] = {0, 0, 0, 0, 0, 0, 0};
  uint32_t lastCheck = millis();
  for (;;) {
    esp_task_wdt_reset();
    const uint32_t now = millis();
    if (now - lastCheck >= 5000) {
      const uint32_t hb[7] = {hbGnss, hbLoRa, hbAudio, hbWeb, hbLoRaWAN, hbBleSensor, hbSensorForward};
      bool stalled = false;
      uint8_t stalledMask = 0;
      for (size_t i = 0; i < 7; ++i) {
        if (hb[i] == last[i]) {
          stalled = true;
          stalledMask |= static_cast<uint8_t>(1U << i);
        }
        last[i] = hb[i];
      }
      StateLock lock(gState);
      if (lock.ok()) {
        gState.heapFree = ESP.getFreeHeap();
        gState.heapLargestFree = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        gState.gnssStackMin = hGnss ? uxTaskGetStackHighWaterMark(hGnss) : 0;
        gState.loraStackMin = hLoRa ? uxTaskGetStackHighWaterMark(hLoRa) : 0;
        gState.audioStackMin = hAudio ? uxTaskGetStackHighWaterMark(hAudio) : 0;
        gState.webStackMin = hWeb ? uxTaskGetStackHighWaterMark(hWeb) : 0;
         gState.lorawanStackMin = hLoRaWAN ? uxTaskGetStackHighWaterMark(hLoRaWAN) : 0;
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
    fuelGauge.task();
    RuntimeConfig config;
    if (!configSnapshot(config)) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (fuelGauge.available()) {
      const float voltage = fuelGauge.voltage();
      const int8_t percent = fuelGauge.percent();
      StateLock batteryLock(gState);
      if (batteryLock.ok()) {
        gState.batteryAvailable = isfinite(voltage);
        gState.batteryV = voltage;
        gState.batteryPercent = percent;
        gState.batteryLow = gState.batteryAvailable &&
                            voltage <= config.batteryLowThreshold;
        gState.batteryCritical = gState.batteryAvailable &&
                                 voltage <= config.batteryCriticalThreshold;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}


static String serialAuthHex(const uint8_t* data, size_t len) {
  static const char digits[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

static bool serialAuthHexDecode(const String& in, uint8_t* out, size_t len) {
  if (!out || in.length() != len * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = nibble(in[i * 2]);
    const int lo = nibble(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

static bool serialAuthExpectedResponse(const RuntimeConfig& cfg, uint8_t out[32]) {
  if (!out || cfg.loraKeyHex.length() != 32) return false;
  uint8_t keyMaterial[32] = {};
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  const char* label = "FieldRadio-Serial-Console-v1";
  bool ok = mbedtls_sha256_starts(&sha, 0) == 0 &&
            mbedtls_sha256_update(&sha,
                                  reinterpret_cast<const unsigned char*>(cfg.loraKeyHex.c_str()),
                                  cfg.loraKeyHex.length()) == 0 &&
            mbedtls_sha256_update(&sha,
                                  reinterpret_cast<const unsigned char*>(label),
                                  std::strlen(label)) == 0 &&
            mbedtls_sha256_finish(&sha, keyMaterial) == 0;
  mbedtls_sha256_free(&sha);
  if (!ok) return false;

  const mbedtls_md_info_t* md =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  return mbedtls_md_hmac(md, keyMaterial, sizeof(keyMaterial),
                         serialAuthChallenge, sizeof(serialAuthChallenge),
                         out, 32) == 0;
}

static void issueSerialAuthChallenge(uint32_t now) {
  for (size_t i = 0; i < sizeof(serialAuthChallenge); ++i) {
    serialAuthChallenge[i] = static_cast<uint8_t>(esp_random() & 0xFFU);
  }
  serialAuthIssuedMs = now;
  serialAuthExpiresMs = now + SERIAL_AUTH_TTL_MS;
  serialAuthenticated = false;
  Serial.printf("AUTH CHALLENGE %s\n",
                serialAuthHex(serialAuthChallenge, sizeof(serialAuthChallenge)).c_str());
}

static bool serialAuthValid(uint32_t now) {
  if (!serialAuthenticated || static_cast<int32_t>(now - serialAuthExpiresMs) >= 0) {
    serialAuthenticated = false;
    return false;
  }
  return true;
}

static bool serialAuthRequired(const String& line) {
  return line != "status" && line != "help" && line != "auth challenge" &&
         !line.startsWith("auth ");
}

static bool serialAuthVerify(const String& responseHex, uint32_t now) {
  if (static_cast<int32_t>(now - serialAuthExpiresMs) >= 0 ||
      now - serialAuthIssuedMs > SERIAL_AUTH_TTL_MS) {
    serialAuthenticated = false;
    return false;
  }

  RuntimeConfig cfg;
  if (!configSnapshot(cfg)) return false;

  uint8_t expected[32] = {};
  uint8_t supplied[32] = {};
  if (!serialAuthExpectedResponse(cfg, expected) ||
      !serialAuthHexDecode(responseHex, supplied, sizeof(supplied))) return false;

  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(expected); ++i) diff |= expected[i] ^ supplied[i];
  if (diff != 0) return false;

  serialAuthenticated = true;
  serialAuthExpiresMs = now + SERIAL_AUTH_TTL_MS;
  return true;
}

static void serviceSerialConsole() {
  static String line;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (line.length() < 128) line += c;
      continue;
    }
    line.trim();
    const uint32_t now = millis();
    if (line == "auth challenge") {
      issueSerialAuthChallenge(now);
    } else if (line.startsWith("auth ")) {
      const String response = line.substring(5);
      Serial.println(serialAuthVerify(response, now) ? "AUTH OK" : "AUTH FAILED");
    } else if (serialAuthRequired(line) && !serialAuthValid(now)) {
      if (!serialAuthExpiresMs || static_cast<int32_t>(now - serialAuthExpiresMs) >= 0)
        issueSerialAuthChallenge(now);
      Serial.println("AUTH REQUIRED; use auth <64-hex-hmac>");
    } else if (line == "status") {
      StateLock lock(gState);
      if (lock.ok()) {
        Serial.printf("LoRa=%d TX=%lu RX=%lu BAT=%.2f %d%% SF=%u LQI=%u\n",
                      gState.loraReady, (unsigned long)gState.txPackets,
                      (unsigned long)gState.rxPackets, gState.batteryV,
                      gState.batteryPercent, lora.currentDataRate(), lora.lqi());
      }
    } else if (line == "config") {
      RuntimeConfig cfg;
      if (!configSnapshot(cfg)) {
        Serial.println("config unavailable");
      } else {
        Serial.printf("freq=%.3f bw=%.1f sf=%u cr=%u pwr=%d callsign=%s\n",
                      cfg.loraFreqMHz, cfg.loraBwKHz, cfg.loraSf,
                      cfg.loraCr, cfg.loraPowerDbm, cfg.callsign.c_str());
      }
    } else if (line == "lw status") {
      Serial.printf("LORAWAN: state=%u joined=%d joining=%d region=%u uplink=%lu downlink=%lu RSSI=%d SNR=%.1f retries=%lu lastJoin=%lu err=%s\n",
                    static_cast<unsigned>(lorawan.state()), lorawan.isJoined(),
                    lorawan.isJoining(), static_cast<unsigned>(lorawan.regionalProfile()),
                    static_cast<unsigned long>(lorawan.uplinkCount()),
                    static_cast<unsigned long>(lorawan.downlinkCount()),
                    lorawan.lastRssi(), lorawan.lastSnr(),
                    static_cast<unsigned long>(lorawan.joinRetryCount()),
                    static_cast<unsigned long>(lorawan.lastJoinAttemptMs()),
                    lorawan.lastError().c_str());
    } else if (line == "lw connect") {
      Serial.println(lorawan.connectOTAA() ? "LORAWAN: join requested" : "LORAWAN: join request rejected");
    } else if (line == "lw disconnect") {
      Serial.println(lorawan.disconnect() ? "LORAWAN: disconnect requested" : "LORAWAN: disconnect rejected");
    } else if (line.startsWith("lw uplink ")) {
      String hex = line.substring(10);
      uint8_t payload[Config::LORAWAN_MAX_PAYLOAD] = {};
      if (hex.length() == 0 || (hex.length() & 1U) ||
          hex.length() > Config::LORAWAN_MAX_PAYLOAD * 2U) {
        Serial.println("LORAWAN: invalid uplink hex");
      } else {
        bool ok = true;
        auto nibble = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        for (size_t i = 0; i < hex.length() / 2; ++i) {
          const int hi = nibble(hex[i * 2]), lo = nibble(hex[i * 2 + 1]);
          if (hi < 0 || lo < 0) { ok = false; break; }
          payload[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        RuntimeConfig cfg;
        const bool haveConfig = configSnapshot(cfg);
        Serial.println(haveConfig &&
                       ok && lorawan.sendUplink(cfg.lorawanFPort, payload, hex.length() / 2)
                           ? "LORAWAN: uplink queued" : "LORAWAN: uplink rejected");
      }
    } else if (line.startsWith("ble passkey ")) {
      const String rest = line.substring(12);
      const int sep = rest.indexOf(' ');
      if (sep <= 0) {
        Serial.println("BLE: usage ble passkey <addr> <passkey>");
      } else {
        String addr = rest.substring(0, sep);
        String pass = rest.substring(sep + 1);
        SensorProtocol::BleAddress parsed{};
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        bool valid = addr.length() == 17 && pass.length() == 6;
        for (size_t i = 0; valid && i < 6; ++i) {
          const size_t pos = (5U - i) * 3U;
          if (i < 5 && addr[pos + 2] != ':') { valid = false; break; }
          const int hi = hex(addr[pos]), lo = hex(addr[pos + 1]);
          if (hi < 0 || lo < 0) { valid = false; break; }
          parsed.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        for (size_t i = 0; valid && i < 6; ++i)
          if (pass[i] < '0' || pass[i] > '9') valid = false;
        const uint32_t key = pass.toInt();
        if (!valid || key < 100000U || key > 999999U ||
            !bleSensorReader.setPeerPasskey(parsed, key))
          Serial.println("BLE: invalid address/passkey or storage failure");
        else
          Serial.println("BLE: peer passkey stored");
      }
    } else if (line.startsWith("ble irk ")) {
      const String rest = line.substring(8);
      const int sep = rest.indexOf(' ');
      SensorProtocol::BleAddress parsed{};
      uint8_t irk[16] = {};
      bool valid = sep == 17 && rest.length() == 17 + 1 + 32;
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const String addr = valid ? rest.substring(0, sep) : String();
      const String key = valid ? rest.substring(sep + 1) : String();
      for (size_t i = 0; valid && i < 6; ++i) {
        const size_t pos = (5U - i) * 3U;
        if (i < 5 && addr[pos + 2] != ':') { valid = false; break; }
        const int hi = hex(addr[pos]), lo = hex(addr[pos + 1]);
        if (hi < 0 || lo < 0) { valid = false; break; }
        parsed.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
      }
      for (size_t i = 0; valid && i < sizeof(irk); ++i) {
        const int hi = hex(key[i * 2]), lo = hex(key[i * 2 + 1]);
        if (hi < 0 || lo < 0) { valid = false; break; }
        irk[i] = static_cast<uint8_t>((hi << 4) | lo);
      }
      if (!valid || !bleSensorReader.setPeerIrk(parsed, irk))
        Serial.println("BLE: invalid IRK/address or storage failure");
      else
        Serial.println("BLE: peer IRK stored");
    } else if (line.startsWith("ble forget ")) {
      const String addr = line.substring(11);
      SensorProtocol::BleAddress parsed{};
      bool valid = addr.length() == 17;
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      for (size_t i = 0; valid && i < 6; ++i) {
        const size_t pos = (5U - i) * 3U;
        if (i < 5 && addr[pos + 2] != ':') { valid = false; break; }
        const int hi = hex(addr[pos]), lo = hex(addr[pos + 1]);
        if (hi < 0 || lo < 0) { valid = false; break; }
        parsed.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
      }
      if (!valid || !bleSensorReader.forgetPeerPasskey(parsed))
        Serial.println("BLE: peer not found or invalid address");
      else
        Serial.println("BLE: peer forgotten");
    } else if (line == "ble list") {
      Serial.println(bleSensorReader.peersJson());
    } else if (line == "cert status") {
      Serial.println(certLifecycle.statusJson());
    } else if (line == "cert renew") {
      Serial.println(certLifecycle.renewCertificate(true) ? "CERT: renewal OK" : "CERT: renewal failed");
    } else if (line == "cert history") {
      Serial.println(certLifecycle.historyJson());
    } else if (line == "est cacerts") {
      Serial.println(certLifecycle.fetchCaChain() ? "EST: CA chain fetched" : "EST: CA chain fetch failed");
    } else if (line == "est csrattrs") {
      Serial.println(certLifecycle.fetchCsrAttrs() ? "EST: CSR attributes fetched" : "EST: CSR attributes fetch failed");
    } else if (line == "reboot") {
      (void)sensorSpool.flush();
      ESP.restart();
    } else if (line == "wipe") {
      nvs_flash_erase();
      Serial.println("NVS erased; reboot required");
    } else if (line == "log") {
      StateLock lock(gState);
      if (lock.ok()) Serial.printf("messages=%u loraLog=%u health=%u\n",
          (unsigned)gState.messageHistoryCount, (unsigned)gState.loraPacketLogCount,
          (unsigned)gState.healthLogCount);
    } else if (line == "help" || line.isEmpty()) {
      Serial.printf("wdt=%lu,%lu,%lu,%lu\n", (unsigned long)gState.wdtResetCounts[0], (unsigned long)gState.wdtResetCounts[1], (unsigned long)gState.wdtResetCounts[2], (unsigned long)gState.wdtResetCounts[3]);
      Serial.println("commands: status auth challenge auth <64-hex-hmac> config cert status cert renew cert history est cacerts est csrattrs ble passkey <addr> <passkey> ble irk <addr> <32-hex> ble forget <addr> ble list lw status lw connect lw disconnect lw uplink <hex> reboot wipe log help");
    } else {
      Serial.println("unknown command; type help");
    }
    line = "";
  }
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  enforceProductionSecurity();
  delay(300);
  Serial.println("\nFieldRadio ESP32-S3-WROOM-1 boot");
#if defined(ARDUINO_ARCH_ESP32)
  if (Board::BUZZER >= 0) pinMode(Board::BUZZER, OUTPUT);
  if (Board::BTN_PTT >= 0) pinMode(Board::BTN_PTT, INPUT_PULLDOWN);
  if (Board::BTN_SOS >= 0) pinMode(Board::BTN_SOS, INPUT_PULLDOWN);
  if (Board::BUZZER >= 0) digitalWrite(Board::BUZZER, LOW);
#endif
  if (detectEmergencyWipeAtBoot()) {
    Serial.println("EMERGENCY WIPE: SOS+PTT held for 10s");
    executeEmergencyWipe();
  }
  gConfigMutex = xSemaphoreCreateMutex();
  if (!gConfigMutex) {
    Serial.println("FATAL: config mutex initialization");
    for (;;) delay(1000);
  }
  gConfig.load();
  if (!configManagerBegin()) {
    Serial.println("FATAL: configuration manager initialization");
    for (;;) delay(1000);
  }
  watchdogInit();

  gState.mutex = xSemaphoreCreateMutex();
  gSpiMutex = xSemaphoreCreateMutex();
  gI2cMutex = xSemaphoreCreateMutex();
  if (!gState.mutex || !gSpiMutex || !gI2cMutex) {
    Serial.println("FATAL: mutex initialization");
    for (;;) delay(1000);
  }
  {
    StateLock lock(gState);
    if (lock.ok()) gState.rangeTest = gConfig.loraRangeTestMode;
  }

  recordBootDiagnostics();
  loadBatteryHealth();
  SPI.begin(Board::SPI_SCK, Board::SPI_MISO, Board::SPI_MOSI);
  Wire.begin(Board::I2C_SDA, Board::I2C_SCL, 400000);
  Wire.setTimeOut(Config::I2C_TIMEOUT_MS);

#if defined(ARDUINO_ARCH_ESP32)
  if (Board::BTN_PTT >= 0) pinMode(Board::BTN_PTT, INPUT_PULLDOWN);
  if (Board::BTN_SOS >= 0) pinMode(Board::BTN_SOS, INPUT_PULLDOWN);
  if (Board::BUZZER >= 0) pinMode(Board::BUZZER, OUTPUT);
  if (Board::HAPTIC >= 0) pinMode(Board::HAPTIC, OUTPUT);
  if (Board::LED_CHARGING >= 0) pinMode(Board::LED_CHARGING, OUTPUT);
  if (Board::LED_TX >= 0) pinMode(Board::LED_TX, OUTPUT);
  if (Board::LED_RX >= 0) pinMode(Board::LED_RX, OUTPUT);
  if (Board::BUZZER >= 0) digitalWrite(Board::BUZZER, LOW);
  if (Board::HAPTIC >= 0) digitalWrite(Board::HAPTIC, LOW);
  if (Board::LED_CHARGING >= 0) digitalWrite(Board::LED_CHARGING, LOW);
  if (Board::LED_TX >= 0) digitalWrite(Board::LED_TX, LOW);
  if (Board::LED_RX >= 0) digitalWrite(Board::LED_RX, LOW);
  rgb.begin();
  rgb.clear();
  rgb.show();
  if (Board::BATTERY_ADC >= 0) {
    pinMode(Board::BATTERY_ADC, INPUT);
    analogSetPinAttenuation(Board::BATTERY_ADC, ADC_11db);
  }
#endif

  bool sdOk = storage.begin();
  const bool sensorSpoolOk = sensorSpool.begin();
  if (!sensorSpoolOk) Serial.println("WARN: sensor spool unavailable; BLE forwarding will remain in RAM until SD recovers");
  bool gpsOk = gnss.begin();
  bool loraOk = lora.begin();
  bool lorawanOk = lorawan.begin();
  bool audioOk = audio.begin();
  const bool batteryGaugeOk = fuelGauge.begin();
  recordBrownoutMarker();
  audio.setVolume(gConfig.volume);

  setupWifi();
  (void)mqtt.begin();
  (void)certLifecycle.begin();
  web.begin();

  const bool usbAudioOk = audio.usbStart();

  bool bleOk = true;
#if SENSOR_READER_ENABLED
  bleOk = bleSensorReader.begin(String("FieldRadio-SensorGateway"));
  if (!bleOk) Serial.println("WARN: BLE sensor reader initialisation deferred/retry enabled");
#endif

  bool tasksOk = true;
  tasksOk &= (xTaskCreatePinnedToCore(taskGnss, "GNSS", 4096, nullptr, 3, &hGnss, 1) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskLoRa, "LoRa", 12288, nullptr, 4, &hLoRa, 1) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskLoRaWAN, "LoRaWAN", 8192, nullptr, 4, &hLoRaWAN, 1) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskNet, "Net", 4096, nullptr, 2, &hNet, 0) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskAudio, "Audio", 8192, nullptr, 5, &hAudio, 0) == pdPASS);
  tasksOk &= (xTaskCreatePinnedToCore(taskWeb, "Web", 6144, nullptr, 2, &hWeb, 0) == pdPASS);
#if SENSOR_READER_ENABLED
  // BLE GATT control task: prio 2, core 0. NimBLE owns its internal
  // host/controller tasks; this task only drives scan/connect/recovery.
  tasksOk &= (xTaskCreatePinnedToCore(taskBleSensor, "BleSensor", 6144, nullptr, 2, &hBleSensor, 0) == pdPASS);
  // Forwarding is isolated from BLE callbacks and runs on core 1.
  tasksOk &= (xTaskCreatePinnedToCore(taskSensorForward, "SensorForward", 4096, nullptr, 2, &hSensorForward, 1) == pdPASS);
#endif
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
  Serial.printf("SD=%d GNSS=%d LoRa=%d LoRaWAN=%d Audio=%d USB=%d WiFi=%d BLE-Sensor=%d\n",
                sdOk, gpsOk, loraOk, lorawanOk, audioOk, usbAudioOk, wifiOk, bleOk);
}

void loop() {
  static bool wdtSubscribed = false;
  if (!wdtSubscribed) {
    watchdogSubscribe();
    wdtSubscribed = true;
  }
  esp_task_wdt_reset();
  serviceSerialConsole();
  const uint32_t now = millis();
  updateBattery(now);
#if defined(ARDUINO_ARCH_ESP32)
  {
    const float tempC = temperatureRead();
    StateLock lock(gState);
    if (lock.ok() && isfinite(tempC)) gState.cpuTempC = tempC;
  }
#endif
  serviceSosBuzzer(now);
  persistRuntimeLogs(now);
  handlePhysicalControls(now);
  bool activePower = false;
  RuntimeConfig powerConfig;
  const bool havePowerConfig = configSnapshot(powerConfig);
  {
    StateLock lock(gState);
    if (lock.ok()) activePower = gState.ptt || gState.sos || gState.recording ||
        gState.playing || gState.usbAudioActive || gState.rxActive ||
        gState.wifiReady || gState.lorawanJoining || gState.lorawanJoined ||
        (havePowerConfig && powerConfig.sensorReaderEnabled && powerConfig.sensorKeepAwake);
  }
  setPowerProfile(activePower);
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
      Serial.printf("DIAG boot=%lu wake=%lu reset=%lu heap=%lu largest=%lu brownout=%d jam=%d noise=%d\n",
                    static_cast<unsigned long>(gState.bootCount),
                    static_cast<unsigned long>(gState.wakeupCause),
                    static_cast<unsigned long>(gState.resetReason),
                    static_cast<unsigned long>(gState.heapFree),
                    static_cast<unsigned long>(gState.heapLargestFree),
                    gState.brownoutReset, gState.jammingDetected, gState.noiseFloorDbm);
    }
  }

  bool lorawanJoined = false;
  {
    StateLock lock(gState);
    if (lock.ok()) lorawanJoined = gState.lorawanJoined;
  }
  if (!lorawanJoined && now - lastReport >= Config::GPS_REPORT_PERIOD_MS) {
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
