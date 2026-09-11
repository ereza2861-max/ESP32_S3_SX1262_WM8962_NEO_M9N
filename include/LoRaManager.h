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
  bool transmit(const String& text, bool alreadyEncrypted = false);
  bool consumeDutyBudget(uint32_t airtimeUs);
  void refillDutyBudget();
  uint64_t dutyTokensUs_ = 0;
  uint32_t lastDutyRefillMs_ = 0;
  uint32_t lastVoiceTxMs_ = 0;
  uint16_t voiceSequence_ = 0;
  uint16_t lastVoiceRxSequence_ = 0;
  bool haveVoiceRxSequence_ = false;
  uint16_t txSequence_ = 0;
  bool encryptPacket(const uint8_t* plain, size_t len, uint8_t type,
                     uint16_t seq, String& packet);
  bool decryptPacket(const String& packet, uint8_t& type, uint16_t& seq,
                     uint8_t* plain, size_t capacity, size_t& len);
  bool loadKey(uint8_t key[16]) const;
  static uint16_t crc16(const uint8_t* data, size_t len);
};
