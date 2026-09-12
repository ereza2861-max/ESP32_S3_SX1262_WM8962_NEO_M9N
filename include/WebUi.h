#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>

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
  File uploadFile_;
  String uploadPath_;
  size_t uploadBytes_ = 0;
  bool uploadFailed_ = false;
  void handleRoot();
  void handleStatus();
  void handleFiles();
  void handleDownload();
  void handleUpload();
  void handleRename();
  void handleMessages();
  void handleLoraLog();
  void handleHealthLog();
  void handleBatteryCalibrate();
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
  void handleTrackDownload();
  void handleReboot();
  bool rateLimit(uint32_t& last, uint32_t interval);
  void handleConfig();
  void handleAudioSource();
};
