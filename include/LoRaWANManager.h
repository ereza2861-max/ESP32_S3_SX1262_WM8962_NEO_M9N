#pragma once
#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "Config.h"

class LoRaManager;

enum class RegionalProfile : uint8_t {
  AS923_1 = 0,
  AS923_2 = 1,
  AS923_3 = 2,
  AS923_4 = 3,
};

enum class LoRaWANState : uint8_t {
  Idle = 0,
  Joining = 1,
  Joined = 2,
  Rejoining = 3,
  Disconnected = 4,
  Error = 5,
};

class LoRaWANManager {
public:
  explicit LoRaWANManager(LoRaManager& p2p);
  bool begin();
  void task();
  bool connectOTAA();
  bool connectABP();
  bool disconnect();
  bool isJoined() const;
  bool isJoining() const;
  LoRaWANState state() const;
  bool sendUplink(uint8_t fPort, const uint8_t* data, size_t len,
                  bool confirmed = false);
  bool sendUplinkText(uint8_t fPort, const String& text,
                      bool confirmed = false);
  bool hasDownlink() const;
  bool popDownlink(uint8_t& fPort, uint8_t* out, size_t& len);
  uint32_t uplinkCount() const;
  uint32_t downlinkCount() const;
  int16_t lastRssi() const;
  float lastSnr() const;
  uint32_t lastJoinAttemptMs() const;
  uint32_t joinRetryCount() const;
  String lastError() const;
  uint8_t currentDataRate() const;
  void setRegionalProfile(RegionalProfile rp);
  RegionalProfile regionalProfile() const;

private:
  struct Downlink {
    uint8_t fPort = 0;
    uint8_t len = 0;
    uint8_t data[Config::LORAWAN_MAX_DOWNLINK] = {};
  };

  LoRaManager& p2p_;
  LoRaWANNode* node_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  QueueHandle_t downlinkQueue_ = nullptr;
  StaticQueue_t downlinkQueueStruct_{};
  uint8_t downlinkQueueStorage_[Config::LORAWAN_DOWNLINK_QUEUE * sizeof(Downlink)] = {};
  volatile LoRaWANState state_ = LoRaWANState::Idle;
  RegionalProfile region_ = static_cast<RegionalProfile>(Config::LORAWAN_REGION_DEFAULT);
  bool ready_ = false;
  bool connectRequested_ = false;
  uint8_t requestedMode_ = 0;
  bool disconnectRequested_ = false;
  uint32_t lastUplinkMs_ = 0;
  uint32_t lastJoinAttemptMs_ = 0;
  uint32_t joinRetryCount_ = 0;
  uint32_t uplinkCount_ = 0;
  uint32_t downlinkCount_ = 0;
  int16_t lastRssi_ = -127;
  float lastSnr_ = -20.0f;
  String lastError_;
  uint32_t retryDelayMs_ = Config::LORAWAN_JOIN_RETRY_MIN_MS;
  bool manualUplinkPending_ = false;
  uint8_t manualFPort_ = Config::LORAWAN_DEFAULT_FPORT;
  uint8_t manualLen_ = 0;
  uint8_t manualPayload_[Config::LORAWAN_MAX_PAYLOAD] = {};
  bool manualConfirmed_ = false;
  uint8_t currentDataRate_ = 0;

  const LoRaWANBand_t* bandForProfile(RegionalProfile rp) const;
  bool recreateNode();
  bool loadNonces();
  bool saveNonces();
  bool loadSession();
  bool saveSession();
  bool parseEui(const String& value, uint64_t& out) const;
  bool parseKey(const String& value, uint8_t out[16]) const;
  bool parseDevAddr(const uint8_t in[4], uint32_t& out) const;
  bool startActivation(uint8_t mode);
  bool performUplink(uint8_t fPort, const uint8_t* data, size_t len, bool confirmed);
  void captureDownlink(const uint8_t* data, size_t len, const LoRaWANEvent_t* event);
  void setError(const String& message);
  void updateState();
  void servicePeriodicTelemetry();
};
