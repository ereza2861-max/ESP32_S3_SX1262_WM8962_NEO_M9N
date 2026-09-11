#pragma once
#include <Arduino.h>
#include <WebServer.h>

class WebUi {
public:
  explicit WebUi(WebServer& server) : server_(server) {}
  void begin();
  void task();
private:
  WebServer& server_;
  bool auth();
  bool sameOrigin();
  uint32_t authWindowStartMs_ = 0;
  uint8_t authFailures_ = 0;
  uint32_t authBlockedUntilMs_ = 0;
  uint32_t lastMessageMs_ = 0;
  uint32_t lastSosMs_ = 0;
  uint32_t lastPttMs_ = 0;
  uint32_t lastConfigMs_ = 0;
  void handleRoot();
  void handleStatus();
  void handleFiles();
  void handleMessage();
  void handleSos();
  void handlePtt();
  void handleRecord();
  void handlePlay();
  void handleStop();
  void handlePause();
  void handleSeek();
  void handleQueue();
  void handleQueueClear();
  void handleRecordPause();
  void handleRecordSplit();
  void handleVox();
  void handleUsbTransport();
  void handleVolume();
  void handleDelete();
  void handleTrack();
  void handleReboot();
  bool rateLimit(uint32_t& last, uint32_t interval);
  void handleConfig();
  void handleAudioSource();
};
