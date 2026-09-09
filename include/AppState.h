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

struct RuntimeState {
  SemaphoreHandle_t mutex = nullptr;
  GpsState gps;
  bool ptt = false;
  bool sos = false;
  bool recording = false;
  bool playing = false;
  bool btStarted = false;
  bool btConnected = false;
  bool loraReady = false;
  bool wifiReady = false;
  bool storageReady = false;
  bool codecReady = false;
  uint8_t volume = 70;
  float batteryV = NAN;
  String lastMessage;
  String lastAudioFile;
  String lastError;
  uint32_t txPackets = 0;
  uint32_t rxPackets = 0;
  uint32_t rxDrops = 0;
  uint32_t audioDrops = 0;
};

extern RuntimeState gState;

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
private:
  RuntimeState& state;
  bool locked;
};
