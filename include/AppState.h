#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Config.h"

struct GpsState {
  double lat = 0.0;
  double lon = 0.0;
  double alt = 0.0;
  uint32_t satellites = 0;
  uint32_t hdop_x10 = 0;
  bool valid = false;
  uint32_t lastFixMs = 0;
  bool timeValid = false;
  uint64_t utcEpoch = 0;
};

struct MessageHistoryEntry {
  uint64_t timestamp = 0;
  uint32_t sourceId = 0;
  String text;
  bool read = false;
};

struct LoraPacketLogEntry {
  uint64_t timestamp = 0;
  bool tx = false;
  uint8_t type = 0;
  uint16_t seq = 0;
  uint32_t sourceId = 0;
  int16_t rssi = -127;
  float snr = -20.0f;
  uint8_t ttl = 0;
};

struct HealthLogEntry {
  uint64_t timestamp = 0;
  uint8_t stalledMask = 0;
  uint32_t heapFree = 0;
  uint32_t gnssStackMin = 0;
  uint32_t loraStackMin = 0;
  uint32_t audioStackMin = 0;
  uint32_t webStackMin = 0;
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
  bool rxActive = false;
  uint32_t rxActivityMs = 0;
  int16_t loraRssi = -127;
  float loraSnr = -20.0f;
  static constexpr size_t RADIO_HISTORY_SIZE = 60;
  int16_t rssiHistory[RADIO_HISTORY_SIZE] = {};
  float snrHistory[RADIO_HISTORY_SIZE] = {};
  uint32_t radioHistoryMs[RADIO_HISTORY_SIZE] = {};
  size_t radioHistoryNext = 0;
  size_t radioHistoryCount = 0;
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
  MessageHistoryEntry messageHistory[Config::MESSAGE_HISTORY_SIZE] = {};
  size_t messageHistoryNext = 0;
  size_t messageHistoryCount = 0;
  size_t messageUnreadCount = 0;
  LoraPacketLogEntry loraPacketLog[Config::LORA_PACKET_LOG_SIZE] = {};
  size_t loraPacketLogNext = 0;
  size_t loraPacketLogCount = 0;
  HealthLogEntry healthLog[Config::HEALTH_LOG_SIZE] = {};
  size_t healthLogNext = 0;
  size_t healthLogCount = 0;
  String lastAudioFile;
  String lastError;
  uint32_t txPackets = 0;
  uint32_t rxPackets = 0;
  uint32_t rxDrops = 0;
  uint32_t audioDrops = 0;
  uint32_t voiceTxPackets = 0;
  uint32_t voiceRxPackets = 0;
  uint32_t voiceDrops = 0;
  uint32_t voiceRxLost = 0;
  uint32_t healthAlerts = 0;
  uint16_t sosSeq = 0;
  bool sosAcked = false;
  uint8_t sosRetries = 0;
  uint32_t sosLastAckMs = 0;
  uint32_t sosLastAckSourceId = 0;
  bool sosEscalated = false;
  uint32_t sosStartedMs = 0;
  uint32_t sosEscalatedMs = 0;
  uint32_t sosAckedBy = 0;
  uint32_t sosBeaconCount = 0;
  uint32_t sosLastBeaconMs = 0;
  struct SosHistoryEntry {
    uint64_t timestamp = 0;
    uint16_t seq = 0;
    uint8_t event = 0; // 0 sent, 1 ack, 2 escalated, 3 cancelled
    uint32_t peer = 0;
  };
  static constexpr size_t SOS_HISTORY_SIZE = 16;
  SosHistoryEntry sosHistory[SOS_HISTORY_SIZE] = {};
  size_t sosHistoryNext = 0;
  size_t sosHistoryCount = 0;
  bool scannerActive = false;
  uint8_t scannerMode = 0;
  uint16_t scannerSweepCount = 0;
  uint8_t scannerChannelCount = 0;
  uint16_t scannerDwellMs = 0;
  uint32_t scannerLastSweepMs = 0;
  bool hopEnabled = false;
  uint8_t hopChannelCount = 0;
  uint8_t hopChannelList[Config::HOP_CHANNEL_MAX] = {};
  uint32_t heapFree = 0;
  uint32_t audioStackMin = 0;
  uint32_t loraStackMin = 0;
  uint32_t gnssStackMin = 0;
  uint32_t webStackMin = 0;
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
