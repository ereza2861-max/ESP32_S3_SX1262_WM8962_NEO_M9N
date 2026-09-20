#pragma once
#include <Arduino.h>
#include <atomic>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "Config.h"
#include "ReplayStore.h"
#include "RfDetector.h"
#include "RadioArbiter.h"
#include "LoRaEcdhRekey.h"

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
  bool textAcked() const {
    return textAcked_.load(std::memory_order_acquire);
  }
  bool sendSOS();
  bool sendPosition();
  bool cancelSOS();
  bool manualTune(float freqMHz);
  bool sendVoiceFrame();
  bool sendSensorTelemetry(uint32_t nodeId, uint16_t sensorId,
                           float value, uint8_t quality,
                           uint64_t timestampMs);
  bool applyConfig();
  void updateSourceId();
  // Arms SX1262 duty-cycle RX before MCU deep sleep. Returns false if the
  // radio cannot be armed safely; caller must not enter deep sleep then.
  bool prepareForDeepSleep();
  bool prepareForFactoryReset();
  void cancelFactoryReset();
  bool scannerStart(uint8_t mode, uint16_t dwellMs);
  bool scannerStop();
  bool scannerIsActive() const;
  void scannerGetResults(struct ChannelScanResult* results, size_t& count);
  size_t scannerSuggestBestChannels(uint8_t* channels, size_t capacity);
  bool persistMessageHistory();
  bool persistFragmentRx();
  bool loadFragmentRx();
  bool loadMessageHistory();
  String neighborsJson() const;
  String routesJson() const;
  bool captureStart(uint32_t durationMs);
  bool captureStop();
  bool captureActive() const;
  String captureDumpJson() const;
  String rangeTestStatusJson() const;
  String ecdhStatusJson() const;
  bool setAdrEnabled(bool enabled);
  bool adrEnabled() const { return adrEnabled_; }
  uint8_t currentDataRate() const { return currentAdrSf_; }
  bool setHopSyncSource(bool gps);
  bool setSosFormats(uint8_t mask);
  bool scheduleMessage(uint64_t atEpoch, const String& text);
  bool cancelScheduledMessage(uint32_t id);
  String scheduledMessagesJson() const;
  bool hopSyncUsesGps() const { return hopSyncGps_; }
  uint32_t forwardDrops() const { return forwardDrops_; }
  uint32_t replayRejects() const { return replayRejects_; }
  uint32_t forwardQueued() const;
  uint32_t forwardLastDropMs() const { return forwardLastDropMs_; }
  uint32_t fragmentEvictions() const { return fragmentEvictions_; }
  uint32_t fragmentDrops() const { return fragmentDrops_; }
  uint64_t dutyBudgetUs() const { return dutyTokensUs_; }
  uint64_t dutyMaxBudgetUs() const;
  uint32_t dedupHits() const { return dedupHits_; }
  uint32_t dedupMisses() const { return dedupMisses_; }
  uint32_t dedupEvictions() const { return dedupEvictions_; }
  uint8_t lqi() const;
  const RfDetector& rfDetector() const { return rfDetector_; }
  PhysicalLayer* radioLayer() { return &radio_; }
  int16_t radioRssi() const { return static_cast<int16_t>(radio_.getRSSI()); }
  float radioSnr() const { return radio_.getSNR(); }
  void suspendForLoRaWAN();
  bool resumeFromLoRaWAN();
private:
  Module module_;
  SX1262 radio_;
  volatile uint32_t irqCount_ = 0;
  portMUX_TYPE irqMux_ = portMUX_INITIALIZER_UNLOCKED;
  bool ready_ = false;
  std::atomic<bool> suspendedForLoRaWAN_{false};
  std::atomic<bool> storageResetting_{false};
  uint32_t lastRecoveryMs_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t seqMutex_ = nullptr;
  SemaphoreHandle_t textStateMutex_ = nullptr;
  static LoRaManager* instance_;
  static void onDio1();
  struct ForwardPacket {
    uint8_t type;
    uint8_t wireVersion = Config::LORA_PROTOCOL_VERSION;
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
  bool sosAckPending_ = false;
  uint16_t sosAckPendingSeq_ = 0;
  uint32_t sosAckPendingSourceId_ = 0;
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
  bool validateTextAckHop(bool hopEnabled) const;
  bool enqueueForward(uint8_t type, uint16_t seq, uint32_t sourceId,
                      uint8_t ttl, const uint8_t* payload, size_t len,
                      uint8_t wireVersion);
  bool seenDedup(uint32_t sourceId, uint16_t seq, uint8_t type, uint32_t payloadHash, uint32_t packetEpochSec = 0);
  static uint32_t hashPayload(const uint8_t* data, size_t len);
  static uint32_t sourceIdFromCallsign(const String& callsign);
  static constexpr size_t ROUTE_EXT_V1_BYTES = 16;
  static constexpr size_t ROUTE_EXT_V2_BYTES = 24;
  static constexpr size_t ROUTE_EXT_BYTES = ROUTE_EXT_V2_BYTES;
  bool addRouteExtension(const uint8_t* payload, size_t len, uint32_t destination,
                         uint32_t excludeNextHop, uint8_t* out, size_t capacity,
                         uint32_t sourceIdOverride = 0) const;
  bool parseRouteExtension(const uint8_t* payload, size_t len, uint32_t& destination,
                           uint32_t& nextHop, uint32_t& previousHop, uint8_t& hopCount,
                           size_t& payloadOffset) const;
  uint16_t calculateEtxQ8(uint16_t attempts, uint16_t success) const;
  uint32_t selectNextHop(uint32_t destination, uint32_t excludeNextHop) const;
  void learnRoute(uint32_t destination, uint32_t nextHop, int16_t rssi, float snr);
  bool routeAllowsForward(uint32_t destination, uint32_t nextHop,
                          uint32_t previousHop) const;
  void recordNeighborTxResult(uint32_t peerSourceId, bool success);
  bool isPttOrRecording() const;
  bool consumeDutyBudget(uint32_t airtimeUs);
  void refundDutyBudget(uint32_t airtimeUs);
  void refillDutyBudget();
  uint64_t dutyTokensUs_ = 0;
  portMUX_TYPE dutyMux_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t lastDutyRefillMs_ = 0;
  uint32_t lastVoiceTxMs_ = 0;
  uint32_t lastForwardTxMs_ = 0;
  uint16_t voiceSequence_ = 0;
  uint16_t lastVoiceRxSequence_ = 0;
  bool haveVoiceRxSequence_ = false;
  uint16_t txSequence_ = 0;
  uint32_t txSequenceAbsolute_ = 1;
  uint32_t txSequenceReservedUntil_ = 0;
  std::atomic<uint32_t> sourceId_{0};
#if FIELDRADIO_LORA_ECDH_REKEY_ENABLED
  struct EcdhPeerState {
    uint32_t sourceId = 0;
    uint32_t epochSec = 0;
    uint8_t ephemeralPublic[LoRaEcdhRekey::PUBLIC_KEY_BYTES] = {};
    uint8_t staticPublic[LoRaEcdhRekey::PUBLIC_KEY_BYTES] = {};
    uint32_t lastSeenMs = 0;
    bool valid = false;
  };
  static constexpr size_t ECDH_PEER_CACHE_SIZE = 16;
  LoRaEcdhRekey::KeyMaterial ecdhKeyMaterial_;
  EcdhPeerState ecdhPeers_[ECDH_PEER_CACHE_SIZE] = {};
  uint32_t ecdhAuthoritativeEpochSec_ = 0;
  uint32_t ecdhKeyEpoch_ = 0;
  bool ecdhActive_ = false;
  bool processEcdhBeacon(uint32_t sourceId, uint32_t packetEpochSec,
                         const uint8_t* payload, size_t len);
#endif
  bool adrEnabled_ = false;
  uint8_t currentAdrSf_ = Config::LORA_SF;
  uint32_t lastAdrMs_ = 0;
  volatile bool messageHistoryDirty_ = false;
  bool hopSyncGps_ = true;
  SemaphoreHandle_t captureMutex_ = nullptr;
  struct CaptureEntry {
    uint32_t ts = 0;
    int16_t rssi = -127;
    float snr = -20.0f;
    bool decrypted = false;
    String rawHex;
  };
  static constexpr size_t CAPTURE_SIZE = 32;
  CaptureEntry capture_[CAPTURE_SIZE] = {};
  size_t captureNext_ = 0;
  size_t captureCount_ = 0;
  uint32_t captureUntilMs_ = 0;
  void serviceRangeTest();
  bool sendRangeTestPacket();
  bool sendRangeTestAck(uint32_t destination, uint32_t counter,
                        uint32_t txTimestampMs);
  void handleRangeTestPayload(uint32_t sourceId, const uint8_t* payload,
                              size_t len, int16_t rssi, float snr);
  std::atomic<uint32_t> rangeTestCounter_{0};
  std::atomic<uint32_t> rangeTestStartMs_{0};
  std::atomic<uint32_t> rangeTestEndMs_{0};
  std::atomic<uint32_t> rangeTestLastTxMs_{0};
  std::atomic<uint32_t> rangeTestTx_{0};
  std::atomic<uint32_t> rangeTestRx_{0};
  std::atomic<uint32_t> rangeTestAck_{0};
  std::atomic<uint32_t> rangeTestLastAckCounter_{0};
  std::atomic<int16_t> rangeTestLastRssi_{-127};
  std::atomic<float> rangeTestLastSnr_{-20.0f};
  std::atomic<bool> rangeTestWasActive_{false};
  bool rangeAckPending_ = false;
  uint32_t rangeAckSourceId_ = 0;
  uint32_t rangeAckCounter_ = 0;
  uint32_t rangeAckTxTimestampMs_ = 0;
  uint32_t replayRejects_ = 0;
  uint32_t dedupHits_ = 0;
  uint32_t dedupMisses_ = 0;
  uint32_t dedupEvictions_ = 0;
  static constexpr size_t SCHEDULED_MESSAGE_MAX = 8;
  struct ScheduledMessage {
    bool used = false;
    uint32_t id = 0;
    uint64_t atEpoch = 0;
    String text;
  };
  struct ScheduledStoreEntry {
    uint8_t used;
    uint8_t reserved[3];
    uint32_t id;
    uint64_t atEpoch;
    char text[192];
  } __attribute__((packed));
  struct ScheduledStore {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t generation;
    uint32_t nextId;
    ScheduledStoreEntry entries[SCHEDULED_MESSAGE_MAX];
    uint32_t crc;
  } __attribute__((packed));
  static constexpr uint32_t SCHEDULED_STORE_MAGIC = 0x53434831UL; // "SCH1"
  static constexpr uint16_t SCHEDULED_STORE_VERSION = 1;
  ScheduledMessage scheduledMessages_[SCHEDULED_MESSAGE_MAX] = {};
  uint32_t nextScheduledMessageId_ = 1;
  uint32_t scheduledStoreGeneration_ = 0;
  bool loadScheduledMessages();
  bool persistScheduledMessages();
  uint8_t sosFormatMask_ = 1; // bit0=text, bit1=APRS-like, bit2=binary
  struct TxQueueEntry {
    bool used = false;
    uint8_t priority = 0;
    uint32_t enqueuedMs = 0;
    String packet;
  };
  TxQueueEntry txQueue_[TX_QUEUE_DEPTH] = {};
  uint32_t lastNeighborBeaconMs_ = 0;
  bool forwardInFlightActive_ = false;
  uint32_t forwardInFlightNextHop_ = 0;
  ForwardPacket forwardInFlight_{};
  uint32_t forwardDrops_ = 0;
  uint32_t forwardLastDropMs_ = 0;
  uint32_t forwardRetryNotBeforeMs_ = 0;
  uint32_t forwardRetryBackoffMs_ = 100;
  uint32_t forwardRetryPersistId_ = 0;
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
  };
  static constexpr size_t FRAGMENT_RX_SLOTS = 3;
  FragmentRxState fragmentRx_[FRAGMENT_RX_SLOTS] = {};
  bool fragmentRxDirty_ = false;
  uint32_t fragmentEvictions_ = 0;
  uint32_t fragmentDrops_ = 0;
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
  ReplayEntry replayCache_[Config::LORA_REPLAY_SOURCE_CACHE_SIZE] = {};
  ReplayStore replayStore_;
  RfDetector rfDetector_;
  bool replayStateLoaded_ = false;
  uint32_t radioRecoveryAttempts_ = 0;
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
      uint32_t generation = 0;
    } results[Config::SCANNER_MAX_CHANNELS] = {};
  } scanner_;
  uint32_t scannerGeneration_ = 0;
  uint32_t hopFrame_ = 0;
  uint32_t hopLastSyncMs_ = 0;
  uint8_t currentHopIndex_ = 0;
  uint8_t legacyRxCounter_ = 0;
  std::atomic<uint16_t> sosSeq_{0};
  uint32_t sosSentMs_ = 0;
  uint8_t sosRetryCount_ = 0;
  bool sosAwaitingAck_ = false;
  bool textAwaitingAck_ = false;
  std::atomic<bool> textAcked_{false};
  uint16_t textPendingSeq_ = 0;
  uint8_t textRetryCount_ = 0;
  uint32_t textSentMs_ = 0;
  String textPendingPacket_;
  bool textAckPending_ = false;
  uint16_t textAckSeq_ = 0;
  uint32_t textAckSourceId_ = 0;
  uint8_t textAckHopIndex_ = 0;
  bool fragmentAckPending_ = false;
  uint32_t fragmentAckSourceId_ = 0;
  uint16_t fragmentAckMessageId_ = 0;
  uint8_t fragmentAckBaseIndex_ = 0;
  uint8_t fragmentAckBitmap_ = 0;
  uint8_t fragmentAckHopIndex_ = 0;
  struct FragmentTxState {
    bool active = false;
    uint32_t destination = 0;
    uint16_t messageId = 0;
    uint8_t count = 0;
    uint16_t ackedMask = 0;
    uint32_t sentMs[Config::LORA_FRAGMENT_MAX_COUNT] = {};
    uint8_t retries[Config::LORA_FRAGMENT_MAX_COUNT] = {};
    uint16_t packetLen[Config::LORA_FRAGMENT_MAX_COUNT] = {};
    uint8_t packets[Config::LORA_FRAGMENT_MAX_COUNT][Config::LORA_MAX_PACKET] = {};
  };
  FragmentTxState fragmentTx_{};
  String sosPacket_;
  uint8_t computeHopIndex(uint32_t frame) const;
  bool retuneToHopChannel(uint8_t index);
  bool retuneToChannel0();
  bool retuneToHopChannelLocked(uint8_t index);
  bool retuneToChannel0Locked();
  void serviceAdr();
  void serviceScheduledMessages();
  bool encryptPacketV3(const uint8_t* plain, size_t len, uint8_t type,
                       uint16_t seq, uint8_t hopIndex, uint32_t epochSec,
                       String& packet);
  bool encryptPacketV5(const uint8_t* plain, size_t len, uint8_t type,
                       uint16_t seq, uint8_t hopIndex, uint32_t epochSec,
                       uint32_t peerSourceId, uint8_t keyEpochDelta,
                       String& packet);
  bool decryptPacketV5(const String& packet, uint8_t& type, uint16_t& seq,
                       uint32_t& sourceId, uint8_t& ttl, uint8_t& hopIndex,
                       uint32_t& epochSec, uint8_t& keyEpochDelta,
                       uint8_t* plain, size_t capacity, size_t& len);
  bool decryptPacketV3(const String& packet, uint8_t& type, uint16_t& seq,
                       uint32_t& sourceId, uint8_t& ttl, uint8_t& hopIndex,
                       uint32_t& epochSec, uint8_t* plain, size_t capacity,
                       size_t& len);
  bool transmitHopped(const String& text, uint8_t type, uint32_t destination = 0);
  bool sendSosAck(uint16_t ackedSeq, uint32_t ackedSourceId);
  void handleSosAckPayload(const uint8_t* payload, size_t len);
  void handleTextAckPayload(const uint8_t* payload, size_t len);
  bool sendTextAck(uint16_t ackedSeq, uint32_t ackedSourceId, uint8_t hopIndex);
  bool sendFragmentAck(uint32_t ackedSourceId, uint16_t messageId,
                       uint8_t baseIndex, uint8_t bitmap, uint8_t hopIndex);
  void handleFragmentAckPayload(const uint8_t* payload, size_t len);
  void serviceTextRetry();
  void serviceFragmentTx();
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
  bool loadReplayState();
  bool persistReplayEntry(const ReplayEntry& entry);
  void hardResetRadio() const;
  bool reserveTxSequenceBlock();
  bool nextTxSequence(uint16_t& seq);
  bool acceptReplay(uint32_t sourceId, uint16_t seq, uint8_t type, uint32_t payloadHash, uint32_t packetEpochSec = 0);
  void updateAntennaHealthAfterTx();
  int8_t effectiveTxPowerDbm() const;
  static uint16_t crc16(const uint8_t* data, size_t len);
  void logPacket(bool tx, uint8_t type, uint16_t seq, uint32_t sourceId,
                 int16_t rssi, float snr, uint8_t ttl);
  void addMessageHistory(uint32_t sourceId, const char* text);
  bool capturePacket(const String& raw, int16_t rssi, float snr, bool decrypted);
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
