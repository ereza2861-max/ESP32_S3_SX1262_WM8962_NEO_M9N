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
  void handleRoot();
  void handleStatus();
  void handleFiles();
  void handleMessage();
  void handleSos();
  void handlePtt();
  void handleRecord();
  void handlePlay();
  void handleStop();
  void handleDelete();
  void handleTrack();
  void handleOta();
};
