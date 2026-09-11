#pragma once
#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

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
  struct ForwardPacket {
    uint8_t type;
    uint8_t ttl;
    uint16_t seq;
    uint32_t sourceId;
    uint32_t dedupId;
    uint32_t receivedMs;
    uint16_t len;
    uint8_t payload[Config::LORA_MAX_PACKET];
  };
  static constexpr size_t FORWARD_QUEUE_DEPTH = Config::LORA_FORWARD_QUEUE_DEPTH;
  static constexpr size_t DEDUP_CACHE_SIZE = Config::LORA_DEDUP_CACHE_SIZE;
  bool transmit(const String& text, bool alreadyEncrypted = false);
  bool transmitForward(const ForwardPacket& packet);
  bool processPendingTx();
  bool lbtChannelBusy(int16_t scanStatus) const;
  bool queuePendingTx(const String& packet);
  bool enqueueForward(uint8_t type, uint16_t seq, uint32_t sourceId,
                      uint8_t ttl, const uint8_t* payload, size_t len);
  bool seenDedup(uint32_t sourceId, uint16_t seq, uint32_t payloadHash);
  static uint32_t hashPayload(const uint8_t* data, size_t len);
  static uint32_t sourceIdFromCallsign(const String& callsign);
  bool isPttOrRecording() const;
  bool consumeDutyBudget(uint32_t airtimeUs);
  void refillDutyBudget();
  uint64_t dutyTokensUs_ = 0;
  uint32_t lastDutyRefillMs_ = 0;
  uint32_t lastVoiceTxMs_ = 0;
  uint32_t lastForwardTxMs_ = 0;
  uint16_t voiceSequence_ = 0;
  uint16_t lastVoiceRxSequence_ = 0;
  bool haveVoiceRxSequence_ = false;
  uint16_t txSequence_ = 0;
  uint32_t sourceId_ = 0;
  struct PendingTx {
    bool active = false;
    String packet;
    uint8_t retries = 0;
    uint32_t nextAttemptMs = 0;
  };
  PendingTx pendingTx_;
  QueueHandle_t forwardQueue_ = nullptr;
  StaticQueue_t forwardQueueStruct_{};
  uint8_t forwardQueueStorage_[FORWARD_QUEUE_DEPTH * sizeof(ForwardPacket)] = {};
  struct DedupEntry {
    uint32_t sourceId;
    uint16_t seq;
    uint32_t payloadHash;
    uint32_t seenMs;
  };
  DedupEntry dedupCache_[DEDUP_CACHE_SIZE] = {};
  size_t dedupNext_ = 0;
  bool encryptPacket(const uint8_t* plain, size_t len, uint8_t type,
                     uint16_t seq, String& packet);
  bool decryptPacket(const String& packet, uint8_t& type, uint16_t& seq,
                     uint32_t& sourceId, uint8_t& ttl, uint8_t* plain,
                     size_t capacity, size_t& len);
  bool loadKey(uint8_t key[16]) const;
  static uint16_t crc16(const uint8_t* data, size_t len);
};
