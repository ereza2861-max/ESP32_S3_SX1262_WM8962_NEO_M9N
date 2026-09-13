#pragma once
#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "Config.h"

struct ChannelScanResult {
  float freqMHz = 0.0f;
  int16_t rssiAvgDbm = -127;
  int16_t rssiPeakDbm = -127;
  float snrDb = -20.0f;
  uint8_t occupancyPercent = 0;
  uint16_t preambleCount = 0;
  uint32_t timestamp = 0;
};

class LoRaManager {
public:
  LoRaManager();
  bool begin();
  void task();
  bool sendText(const String& text);
  bool sendTextTo(uint32_t destination, const String& text);
  bool textAcked() const { return textAcked_; }
  bool sendSOS();
  bool sendPosition();
  bool cancelSOS();
  bool manualTune(float freqMHz);
  bool sendVoiceFrame();
  bool applyConfig();
  void prepareForDeepSleep();
  bool scannerStart(uint8_t mode, uint16_t dwellMs);
  bool scannerStop();
  bool scannerIsActive() const;
  void scannerGetResults(struct ChannelScanResult* results, size_t& count);
  size_t scannerSuggestBestChannels(uint8_t* channels, size_t capacity);
private:
  Module module_;
  SX1262 radio_;
  volatile uint32_t irqCount_ = 0;
  portMUX_TYPE irqMux_ = portMUX_INITIALIZER_UNLOCKED;
  bool ready_ = false;
  uint32_t lastRecoveryMs_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t seqMutex_ = nullptr;
  static LoRaManager* instance_;
  static void onDio1();
  struct ForwardPacket {
    uint8_t type;
    uint8_t ttl;
    uint16_t seq;
    uint32_t sourceId;
    uint32_t dedupId;
    uint32_t receivedMs;
    uint8_t priority;
    uint32_t persistId = 0;
    uint16_t len;
    uint8_t payload[Config::LORA_MAX_PACKET];
  };
  static constexpr size_t FORWARD_QUEUE_DEPTH = Config::LORA_FORWARD_QUEUE_DEPTH;
  static constexpr size_t TX_QUEUE_DEPTH = Config::LORA_TX_QUEUE_DEPTH;
  static constexpr size_t DEDUP_CACHE_SIZE = Config::LORA_DEDUP_CACHE_SIZE;
  bool transmit(const String& text, bool alreadyEncrypted = false);
  bool transmitForward(const ForwardPacket& packet);
  bool processPendingTx();
  bool queuePendingTx(const String& packet, uint8_t priority = 0);
  void serviceVoiceAckRetry();
  bool voiceAckPending_ = false;
  uint16_t voiceAckPendingSeq_ = 0;
  uint32_t voiceAckPendingSourceId_ = 0;
  int16_t voiceAckPendingRssi_ = -127;
  float voiceAckPendingSnr_ = -20.0f;
  void serviceNeighborBeacon();
  bool sendVoiceAck(uint16_t ackedSeq, uint32_t ackedSourceId, int16_t rssi, float snr);
  void handleVoiceAckPayload(uint32_t ackSenderSourceId, const uint8_t* payload, size_t len);
  void updateNeighborMetric(uint32_t sourceId, int16_t rssi, float snr);
  uint8_t neighborQualityForPeer(uint32_t sourceId) const;
  uint8_t bestNeighborQuality() const;
  bool persistForwardQueue();
  bool loadForwardQueue();
  bool enqueueTextFragments(const String& text, uint32_t destination = 0);
  bool handleTextFragment(uint32_t sourceId, const uint8_t* payload, size_t len);
  static uint8_t txPriorityForPacket(const String& packet);
  bool lbtChannelBusy(int16_t scanStatus) const;
  bool enqueueForward(uint8_t type, uint16_t seq, uint32_t sourceId,
                      uint8_t ttl, const uint8_t* payload, size_t len);
  bool seenDedup(uint32_t sourceId, uint16_t seq, uint8_t type, uint32_t payloadHash, uint32_t packetEpochSec = 0);
  static uint32_t hashPayload(const uint8_t* data, size_t len);
  static uint32_t sourceIdFromCallsign(const String& callsign);
  static constexpr size_t ROUTE_EXT_BYTES = 16;
  bool addRouteExtension(const uint8_t* payload, size_t len, uint32_t destination,
                         uint32_t excludeNextHop, uint8_t* out, size_t capacity) const;
  bool parseRouteExtension(const uint8_t* payload, size_t len, uint32_t& destination,
                           uint32_t& nextHop, uint32_t& previousHop, uint8_t& hopCount,
                           size_t& payloadOffset) const;
  uint32_t selectNextHop(uint32_t destination, uint32_t excludeNextHop) const;
  void learnRoute(uint32_t destination, uint32_t nextHop, int16_t rssi, float snr);
  bool routeAllowsForward(uint32_t destination, uint32_t nextHop,
                          uint32_t previousHop) const;
  void recordNeighborTxResult(uint32_t peerSourceId, bool success);
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
  uint32_t txSequenceAbsolute_ = 1;
  uint32_t txSequenceReservedUntil_ = 0;
  uint32_t sourceId_ = 0;
  struct TxQueueEntry {
    bool used = false;
    uint8_t priority = 0;
    uint32_t enqueuedMs = 0;
    String packet;
  };
  TxQueueEntry txQueue_[TX_QUEUE_DEPTH] = {};
  uint32_t lastNeighborBeaconMs_ = 0;
  bool forwardInFlightActive_ = false;
  ForwardPacket forwardInFlight_{};
  uint16_t fragmentMessageId_ = 0;
  struct VoiceTxSlot {
    bool used = false;
    bool acked = false;
    uint16_t seq = 0;
    String packet;
    uint8_t retries = 0;
    uint32_t sentMs = 0;
    uint32_t nextAttemptMs = 0;
    uint32_t peerSourceId = 0;
    uint8_t peerQuality = 0;
  };
  VoiceTxSlot voiceTx_[Config::LORA_VOICE_WINDOW_SIZE] = {};
  uint8_t voiceTxOutstanding_ = 0;
  bool voiceRxAckInitialized_ = false;
  uint32_t voiceRxAckSourceId_ = 0;
  uint16_t voiceRxAckBase_ = 0;
  uint8_t voiceRxAckBitmap_ = 0;
  struct FragmentRxState {
    bool active = false;
    uint32_t sourceId = 0;
    uint16_t messageId = 0;
    uint8_t count = 0;
    uint16_t totalLen = 0;
    uint16_t receivedMask = 0;
    uint16_t receivedBytes = 0;
    uint32_t startedMs = 0;
    uint8_t data[Config::LORA_FRAGMENT_MAX_BYTES] = {};
    uint16_t lengths[Config::LORA_FRAGMENT_MAX_COUNT] = {};
  } fragmentRx_;
  struct NeighborEntry {
    uint32_t sourceId = 0;
    int16_t rssi = -127;
    float snr = -20.0f;
    uint32_t seenMs = 0;
    uint8_t quality = 0;
    uint16_t txAttempts = 0;
    uint16_t txSuccess = 0;
  };
  struct RouteEntry {
    uint32_t destination = 0;
    uint32_t nextHop = 0;
    uint16_t etxQ8 = 256;
    uint8_t quality = 0;
    uint32_t seenMs = 0;
  };
  static constexpr size_t NEIGHBOR_CACHE_SIZE = 16;
  static constexpr size_t ROUTE_CACHE_SIZE = 16;
  NeighborEntry neighbors_[NEIGHBOR_CACHE_SIZE] = {};
  size_t neighborNext_ = 0;
  RouteEntry routes_[ROUTE_CACHE_SIZE] = {};
  size_t routeNext_ = 0;
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
  struct ReplayEntry {
    uint32_t sourceId = 0;
    uint16_t highestSeq = 0;
    uint8_t type = 0;
    uint32_t bitmap = 0;
    uint32_t highestPayloadHash = 0;
    uint32_t seenMs = 0;
    uint32_t lastEpochSec = 0;
  };
  ReplayEntry replayCache_[Config::LORA_REPLAY_SOURCE_CACHE_SIZE] = {};
  size_t replayNext_ = 0;
  struct ScannerState {
    bool active = false;
    uint8_t mode = 0;
    uint16_t dwellMs = Config::SCANNER_DEFAULT_DWELL_MS;
    uint8_t index = 0;
    uint16_t sweepCount = 0;
    uint32_t lastSampleMs = 0;
    struct Result {
      float freqMHz = 0.0f;
      int16_t rssiAvgDbm = -127;
      int16_t rssiPeakDbm = -127;
      float snrDb = -20.0f;
      uint8_t occupancyPercent = 0;
      uint16_t preambleCount = 0;
      uint32_t timestamp = 0;
    } results[Config::SCANNER_MAX_CHANNELS] = {};
  } scanner_;
  uint32_t hopFrame_ = 0;
  uint32_t hopLastSyncMs_ = 0;
  uint8_t currentHopIndex_ = 0;
  uint8_t legacyRxCounter_ = 0;
  uint16_t sosSeq_ = 0;
  uint32_t sosSentMs_ = 0;
  uint8_t sosRetryCount_ = 0;
  bool sosAwaitingAck_ = false;
  bool textAwaitingAck_ = false;
  bool textAcked_ = false;
  uint16_t textPendingSeq_ = 0;
  uint8_t textRetryCount_ = 0;
  uint32_t textSentMs_ = 0;
  String textPendingPacket_;
  bool textAckPending_ = false;
  uint16_t textAckSeq_ = 0;
  uint32_t textAckSourceId_ = 0;
  uint8_t textAckHopIndex_ = 0;
  String sosPacket_;
  uint8_t computeHopIndex(uint32_t frame) const;
  bool retuneToHopChannel(uint8_t index);
  bool retuneToChannel0();
  bool encryptPacketV3(const uint8_t* plain, size_t len, uint8_t type,
                       uint16_t seq, uint8_t hopIndex, uint32_t epochMs,
                       String& packet);
  bool decryptPacketV3(const String& packet, uint8_t& type, uint16_t& seq,
                       uint32_t& sourceId, uint8_t& ttl, uint8_t& hopIndex,
                       uint32_t& epochMs, uint8_t* plain, size_t capacity,
                       size_t& len);
  bool transmitHopped(const String& text, uint8_t type, uint32_t destination = 0);
  bool sendSosAck(uint16_t ackedSeq, uint32_t ackedSourceId);
  void handleSosAckPayload(const uint8_t* payload, size_t len);
  void handleTextAckPayload(const uint8_t* payload, size_t len);
  bool sendTextAck(uint16_t ackedSeq, uint32_t ackedSourceId, uint8_t hopIndex);
  void serviceTextRetry();
  void serviceSosRetry();
  void addSosHistory(uint8_t event, uint32_t peer = 0);
  bool encryptPacket(const uint8_t* plain, size_t len, uint8_t type,
                     uint16_t seq, String& packet);
  bool encryptRoutedPacket(const uint8_t* plain, size_t len, uint8_t type,
                           uint16_t seq, uint32_t destination,
                           uint32_t excludeNextHop, String& packet);
  bool decryptPacket(const String& packet, uint8_t& type, uint16_t& seq,
                     uint32_t& sourceId, uint8_t& ttl, uint8_t* plain,
                     size_t capacity, size_t& len);
  bool loadKey(uint8_t key[16]) const;
  bool reserveTxSequenceBlock();
  bool nextTxSequence(uint16_t& seq);
  bool acceptReplay(uint32_t sourceId, uint16_t seq, uint8_t type, uint32_t payloadHash, uint32_t packetEpochSec = 0);
  int8_t effectiveTxPowerDbm() const;
  static uint16_t crc16(const uint8_t* data, size_t len);
  void logPacket(bool tx, uint8_t type, uint16_t seq, uint32_t sourceId,
                 int16_t rssi, float snr, uint8_t ttl);
  void addMessageHistory(uint32_t sourceId, const char* text);
  bool forwardRateAllowed(uint32_t sourceId, uint8_t type);
  void serviceVoiceReorder();
  struct ForwardSourceRate {
    uint32_t sourceId = 0;
    uint32_t lastMs = 0;
  };
  ForwardSourceRate forwardSourceRates_[Config::LORA_FORWARD_SOURCE_CACHE_SIZE] = {};
  size_t forwardSourceNext_ = 0;
  struct VoiceRxSlot {
    bool used = false;
    uint16_t seq = 0;
    uint32_t receivedMs = 0;
    uint8_t data[168] = {};
  };
  VoiceRxSlot voiceRx_[Config::VOICE_REORDER_BUFFER_SIZE] = {};
};
