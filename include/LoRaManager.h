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
  bool sendVoiceFrame();
  bool applyConfig();
private:
  Module module_;
  SX1276 radio_;
  volatile uint32_t irqCount_ = 0;
  portMUX_TYPE irqMux_ = portMUX_INITIALIZER_UNLOCKED;
  bool ready_ = false;
  uint32_t lastRecoveryMs_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
  static LoRaManager* instance_;
  static void onDio0();
  bool transmitLocked(const String& text);
  bool consumeDutyBudget(uint32_t airtimeUs);
  void refillDutyBudget();
  uint64_t dutyTokensUs_ = 0;
  uint32_t lastDutyRefillMs_ = 0;
  uint32_t lastVoiceTxMs_ = 0;
  uint16_t voiceSequence_ = 0;
};
