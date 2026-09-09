#pragma once
#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class LoRaManager {
public:
  LoRaManager();
  bool begin();
  void task();
  bool sendText(const String& text);
  bool sendSOS();
  bool sendPosition();
private:
  Module module_;
  SX1276 radio_;
  volatile bool irqFlag_ = false;
  bool ready_ = false;
  SemaphoreHandle_t mutex_ = nullptr;
  static LoRaManager* instance_;
  static void onDio0();
  bool transmitLocked(const String& text);
};
