#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct GpsState {
  double lat = 0.0;
  double lon = 0.0;
  double alt = 0.0;
  uint32_t satellites = 0;
  uint32_t hdop_x10 = 0;
  bool valid = false;
  uint32_t lastFixMs = 0;
};

extern SemaphoreHandle_t gSpiMutex;

struct RuntimeState {
  SemaphoreHandle_t mutex = nullptr;
  GpsState gps;
  bool ptt = false;
  bool sos = false;
  bool recording = false;
  bool recordingPaused = false;
  bool vox = false;
  bool playing = false;
  bool playbackPaused = false;
  uint32_t playbackPositionMs = 0;
  uint32_t playbackDurationMs = 0;
  uint8_t queueDepth = 0;
  bool usbAudioReady = false;
  bool usbAudioActive = false;
  bool loraReady = false;
  bool wifiReady = false;
  bool storageReady = false;
  bool codecReady = false;
  uint8_t volume = 70;
  bool usbMuted = false;
  uint8_t usbVolume = 100;
  bool usbMonitor = false;
  bool usbPlaybackTransport = false;
  bool aecEnabled = false;
  uint32_t usbSampleRate = 44100;
  bool audioLoopback = false;
  float audioPeak = 0.0f;
  float audioRms = 0.0f;
  bool audioClipped = false;
  float batteryV = NAN;
  bool batteryAvailable = false;
  bool batteryLow = false;
  bool batteryCritical = false;
  String lastMessage;
  String lastAudioFile;
  String lastError;
  uint32_t txPackets = 0;
  uint32_t rxPackets = 0;
  uint32_t rxDrops = 0;
  uint32_t audioDrops = 0;
  uint32_t voiceTxPackets = 0;
  uint32_t voiceRxPackets = 0;
  uint32_t voiceDrops = 0;
};

extern RuntimeState gState;

class SpiLock {
public:
  explicit SpiLock(TickType_t timeout = pdMS_TO_TICKS(100))
      : locked_(gSpiMutex && xSemaphoreTake(gSpiMutex, timeout) == pdTRUE) {}
  ~SpiLock() { if (locked_) xSemaphoreGive(gSpiMutex); }
  bool ok() const { return locked_; }
  SpiLock(const SpiLock&) = delete;
  SpiLock& operator=(const SpiLock&) = delete;
private:
  bool locked_;
};

class StateLock {
public:
  explicit StateLock(RuntimeState& s, TickType_t timeout = pdMS_TO_TICKS(50))
      : state(s), locked(false) {
    if (state.mutex) {
      locked = xSemaphoreTake(state.mutex, timeout) == pdTRUE;
    }
  }
  ~StateLock() {
    if (locked) xSemaphoreGive(state.mutex);
  }
  bool ok() const { return locked; }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
private:
  RuntimeState& state;
  bool locked;
};
