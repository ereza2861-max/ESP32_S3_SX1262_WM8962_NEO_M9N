#include "LoRaWANManager.h"
#include "LoRaManager.h"
#include "PersistentConfig.h"
#include "AppState.h"
#include "Telemetry.h"
#include "RadioArbiter.h"
#include <Preferences.h>
#include <esp_system.h>
#include <cstdlib>
#include <cstring>

namespace {
constexpr char NVS_NS[] = "fieldradio";
constexpr char NVS_NONCES_KEY[] = "lw_nonces";
constexpr char NVS_NONCES_VERSION_KEY[] = "lw_nonce_v";
constexpr char NVS_SESSION_KEY[] = "lw_session";
constexpr char NVS_SESSION_VERSION_KEY[] = "lw_session_v";
constexpr uint8_t NVS_NONCES_VERSION = 1;
constexpr uint8_t NVS_SESSION_VERSION = 1;
}

LoRaWANManager::LoRaWANManager(LoRaManager& p2p) : p2p_(p2p) {}

const LoRaWANBand_t* LoRaWANManager::bandForProfile(RegionalProfile rp) const {
  switch (rp) {
    case RegionalProfile::AS923_1: return &AS923;
    case RegionalProfile::AS923_2: return &AS923_2;
    case RegionalProfile::AS923_3: return &AS923_3;
    case RegionalProfile::AS923_4: return &AS923_4;
    default: return nullptr;
  }
}

bool LoRaWANManager::recreateNode() {
  const LoRaWANBand_t* band = bandForProfile(region_);
  if (!band) return false;
  if (node_) {
    delete node_;
    node_ = nullptr;
  }
  node_ = new LoRaWANNode(p2p_.radioLayer(), band, 0);
  if (!node_) return false;
  node_->setDutyCycle(Config::LORAWAN_DUTY_CYCLE_ENABLED);
  node_->setDwellTime(Config::LORAWAN_DWELL_TIME_ENABLED,
                      Config::LORAWAN_MAX_DWELL_MS);
  node_->setADR(true);
  return true;
}

static String deviceDevEuiFromEfuse() {
  uint64_t mac = ESP.getEfuseMac();
  char eui[17] = {};
  snprintf(eui, sizeof(eui), "%016llX",
           static_cast<unsigned long long>(mac));
  return String(eui);
}

bool LoRaWANManager::begin() {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (ready_) return true;
  mutex_ = xSemaphoreCreateMutex();
  downlinkQueue_ = xQueueCreateStatic(
      Config::LORAWAN_DOWNLINK_QUEUE, sizeof(Downlink),
      downlinkQueueStorage_, &downlinkQueueStruct_);
  if (!mutex_ || !downlinkQueue_) {
    setError("LoRaWAN RTOS init failed");
    return false;
  }

#ifndef CONFIG_NVS_ENCRYPTION
#define CONFIG_NVS_ENCRYPTION 0
#endif
#if !CONFIG_NVS_ENCRYPTION
  Serial.println("LORAWAN: WARNING: CONFIG_NVS_ENCRYPTION is disabled; credentials are not protected at rest");
#endif

  // DevEUI is hardware-derived; never mutate RuntimeConfig from the LoRaWAN task.
  hardwareDevEui_ = deviceDevEuiFromEfuse();
  region_ = static_cast<RegionalProfile>(config.lorawanRegion <= 3 ?
                                          config.lorawanRegion :
                                          Config::LORAWAN_REGION_DEFAULT);
  if (!recreateNode()) {
    setError("LoRaWAN node init failed");
    return false;
  }
  loadNonces();
  ready_ = true;
  state_ = LoRaWANState::Disconnected;
  if (config.lorawanEnabled) {
    requestedMode_ = config.lorawanMode;
    connectRequested_ = true;
    state_ = LoRaWANState::Joining;
    lastJoinAttemptMs_ = millis();
  }
  updateState();
  Serial.printf("LORAWAN: ready region=%u enabled=%d\n",
                static_cast<unsigned>(region_), config.lorawanEnabled);
  return true;
}

bool LoRaWANManager::parseEui(const String& value, uint64_t& out) const {
  if (value.length() != 16) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return false;
  }
  char* end = nullptr;
  const unsigned long long parsed = strtoull(value.c_str(), &end, 16);
  if (!end || *end != '\0') return false;
  out = static_cast<uint64_t>(parsed);
  return true;
}

bool LoRaWANManager::parseKey(const String& value, uint8_t out[16]) const {
  if (!out || value.length() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = nibble(value[i * 2]);
    const int lo = nibble(value[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool LoRaWANManager::parseDevAddr(const uint8_t in[4], uint32_t& out) const {
  if (!in) return false;
  memcpy(&out, in, sizeof(out));
  return out != 0;
}

bool LoRaWANManager::loadNonces() {
  if (!node_) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  const uint8_t version = prefs.getUChar(NVS_NONCES_VERSION_KEY, 0);
  if (version != NVS_NONCES_VERSION) {
    prefs.end();
    return true;
  }
  uint8_t buffer[RADIOLIB_LORAWAN_NONCES_BUF_SIZE] = {};
  const size_t got = prefs.getBytes(NVS_NONCES_KEY, buffer, sizeof(buffer));
  prefs.end();
  if (got != sizeof(buffer)) return true;
  return node_->setBufferNonces(buffer) == RADIOLIB_ERR_NONE;
}

bool LoRaWANManager::saveNonces() {
  if (!node_) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  const uint8_t* buffer = node_->getBufferNonces();
  const size_t n = prefs.putBytes(NVS_NONCES_KEY, buffer,
                                  RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  const size_t v = prefs.putUChar(NVS_NONCES_VERSION_KEY, NVS_NONCES_VERSION);
  prefs.end();
  return n == RADIOLIB_LORAWAN_NONCES_BUF_SIZE && v == sizeof(uint8_t);
}

bool LoRaWANManager::loadSession() {
  if (!node_) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  const uint8_t version = prefs.getUChar(NVS_SESSION_VERSION_KEY, 0);
  if (version != NVS_SESSION_VERSION) {
    prefs.end();
    return false;
  }
  uint8_t buffer[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = {};
  const size_t got = prefs.getBytes(NVS_SESSION_KEY, buffer, sizeof(buffer));
  prefs.end();
  if (got != sizeof(buffer)) return false;
  return node_->setBufferSession(buffer) == RADIOLIB_ERR_NONE;
}

bool LoRaWANManager::saveSession() {
  if (!node_) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  const uint8_t* buffer = node_->getBufferSession();
  const size_t n = prefs.putBytes(NVS_SESSION_KEY, buffer,
                                  RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  const size_t v = prefs.putUChar(NVS_SESSION_VERSION_KEY, NVS_SESSION_VERSION);
  prefs.end();
  return n == RADIOLIB_LORAWAN_SESSION_BUF_SIZE && v == sizeof(uint8_t);
}

void LoRaWANManager::setError(const String& message) {
  lastError_ = message;
  state_ = LoRaWANState::Error;
  StateLock lock(gState);
  if (lock.ok()) gState.lorawanLastError = message;
  Serial.printf("LORAWAN: %s\n", message.c_str());
}

void LoRaWANManager::updateState() {
  StateLock lock(gState);
  if (!lock.ok()) return;
  gState.lorawanReady = ready_;
  gState.lorawanJoined = state_ == LoRaWANState::Joined;
  gState.lorawanJoining = state_ == LoRaWANState::Joining ||
                          state_ == LoRaWANState::Rejoining;
  gState.lorawanState = static_cast<uint8_t>(state_);
  gState.lorawanUplinkCount = uplinkCount_;
  gState.lorawanDownlinkCount = downlinkCount_;
  gState.lorawanRssi = lastRssi_;
  gState.lorawanSnr = lastSnr_;
  gState.lorawanJoinRetryCount = joinRetryCount_;
  gState.lorawanLastJoinMs = lastJoinAttemptMs_;
  gState.lorawanRegion = static_cast<uint8_t>(region_);
  gState.lorawanDataRate = currentDataRate_;
  gState.lorawanLastError = lastError_;
  if (hardwareDevEui_.length() == 16) {
    gState.lorawanDevEuiMasked =
        String("****") + hardwareDevEui_.substring(12);
  } else {
    gState.lorawanDevEuiMasked = "";
  }
}

bool LoRaWANManager::startActivation(uint8_t mode) {
  RuntimeConfig config{}; if (!configSnapshot(config)) return false;
  if (!ready_ || !node_ || !config.lorawanEnabled) {
    setError("LoRaWAN is disabled or not initialized");
    return false;
  }
  if (!config.validLoRaWAN()) {
    setError("Invalid LoRaWAN configuration");
    return false;
  }

  uint64_t devEui = 0;
  if (!parseEui(hardwareDevEui_, devEui)) {
    setError("Invalid DevEUI");
    return false;
  }

  int16_t st = RADIOLIB_ERR_UNKNOWN;
  if (mode == 0) {
    uint64_t joinEui = 0;
    uint8_t appKey[16] = {};
    if (!parseEui(config.lorawanJoinEui, joinEui) ||
        !parseKey(config.lorawanAppKey, appKey)) {
      setError("Invalid OTAA credentials");
      return false;
    }
    // LoRaWAN 1.0.x uses the same AppKey for the network and application
    // root keys. RadioLib performs the join MIC/session-key derivation.
    st = node_->beginOTAA(joinEui, devEui, appKey, appKey);
    if (st == RADIOLIB_ERR_NONE) {
      // A persisted session lets RadioLib resume without resetting FCntUp/FCntDown.
      // If the session is absent or stale, activateOTAA() will establish a new one.
      (void)loadSession();
      LoRaWANJoinEvent_t joinEvent{};
      st = node_->activateOTAA(&joinEvent);
      // DevNonce is advanced as soon as a JoinRequest is transmitted, even when
      // the join is rejected. Persist it on every activation attempt.
      if (!saveNonces()) {
        setError("LoRaWAN nonce persistence failed");
        connectRequested_ = false;
        return false;
      }
    }
  } else {
    uint8_t nwkKey[16] = {};
    uint8_t appSKey[16] = {};
    uint32_t devAddr = 0;
    if (!parseKey(config.lorawanNwkSKey, nwkKey) ||
        !parseKey(config.lorawanAppSKey, appSKey) ||
        !parseDevAddr(config.lorawanDevAddr, devAddr)) {
      setError("Invalid ABP credentials");
      return false;
    }
    // LoRaWAN 1.0.x has one NwkSKey. RadioLib's 1.1-compatible ABP API
    // accepts the same key for FNwkSIntKey, SNwkSIntKey and NwkSEncKey.
    st = node_->beginABP(devAddr, nwkKey, nwkKey, nwkKey, appSKey);
    if (st == RADIOLIB_ERR_NONE) {
      (void)loadSession();
      st = node_->activateABP();
    }
  }

  const bool activationOk =
      st == RADIOLIB_ERR_NONE ||
      st == RADIOLIB_LORAWAN_NEW_SESSION ||
      st == RADIOLIB_LORAWAN_SESSION_RESTORED;
  if (!activationOk) {
    setError("Activation failed: " + String(st));
    return false;
  }

  if (!saveSession()) {
    setError("LoRaWAN session persistence failed");
    connectRequested_ = false;
    return false;
  }

  state_ = LoRaWANState::Joined;
  retryDelayMs_ = Config::LORAWAN_JOIN_RETRY_MIN_MS;
  lastError_ = "";
  lastRssi_ = p2p_.radioRssi();
  lastSnr_ = p2p_.radioSnr();
  updateState();
  Serial.println("LORAWAN: joined/activated");
  return true;
}

bool LoRaWANManager::connectOTAA() {
  if (!mutex_ || !ready_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  requestedMode_ = 0;
  connectRequested_ = true;
  disconnectRequested_ = false;
  state_ = LoRaWANState::Joining;
  lastJoinAttemptMs_ = millis();
  updateState();
  xSemaphoreGive(mutex_);
  return true;
}

bool LoRaWANManager::connectABP() {
  if (!mutex_ || !ready_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  requestedMode_ = 1;
  connectRequested_ = true;
  disconnectRequested_ = false;
  state_ = LoRaWANState::Joining;
  lastJoinAttemptMs_ = millis();
  updateState();
  xSemaphoreGive(mutex_);
  return true;
}

bool LoRaWANManager::disconnect() {
  if (!mutex_ || !ready_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  connectRequested_ = false;
  disconnectRequested_ = true;
  manualUplinkPending_ = false;
  state_ = LoRaWANState::Disconnected;
  updateState();
  xSemaphoreGive(mutex_);
  return true;
}

bool LoRaWANManager::isJoined() const {
  return state_ == LoRaWANState::Joined;
}

bool LoRaWANManager::isJoining() const {
  return state_ == LoRaWANState::Joining || state_ == LoRaWANState::Rejoining;
}

LoRaWANState LoRaWANManager::state() const {
  return state_;
}

bool LoRaWANManager::sendUplink(uint8_t fPort, const uint8_t* data,
                                size_t len, bool confirmed) {
  if (!data || len == 0 || len > Config::LORAWAN_MAX_PAYLOAD ||
      fPort == 0 || fPort > 223 || state_ != LoRaWANState::Joined || !mutex_)
    return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  if (manualUplinkPending_) {
    xSemaphoreGive(mutex_);
    return false;
  }
  memcpy(manualPayload_, data, len);
  manualLen_ = static_cast<uint8_t>(len);
  manualFPort_ = fPort;
  manualConfirmed_ = confirmed;
  manualUplinkPending_ = true;
  xSemaphoreGive(mutex_);
  return true;
}

bool LoRaWANManager::sendUplinkText(uint8_t fPort, const String& text,
                                     bool confirmed) {
  if (text.isEmpty() || text.length() > Config::LORAWAN_MAX_PAYLOAD) return false;
  return sendUplink(fPort,
                    reinterpret_cast<const uint8_t*>(text.c_str()),
                    text.length(), confirmed);
}

bool LoRaWANManager::hasDownlink() const {
  return downlinkQueue_ && uxQueueMessagesWaiting(downlinkQueue_) > 0;
}

bool LoRaWANManager::popDownlink(uint8_t& fPort, uint8_t* out, size_t& len) {
  if (!out || !downlinkQueue_) return false;
  Downlink item{};
  if (xQueuePeek(downlinkQueue_, &item, 0) != pdPASS) return false;
  if (len < item.len) return false;
  if (xQueueReceive(downlinkQueue_, &item, 0) != pdPASS) return false;
  fPort = item.fPort;
  len = item.len;
  memcpy(out, item.data, item.len);
  return true;
}

uint32_t LoRaWANManager::uplinkCount() const { return uplinkCount_; }
uint32_t LoRaWANManager::downlinkCount() const { return downlinkCount_; }
int16_t LoRaWANManager::lastRssi() const { return lastRssi_; }
float LoRaWANManager::lastSnr() const { return lastSnr_; }
uint32_t LoRaWANManager::lastJoinAttemptMs() const { return lastJoinAttemptMs_; }
uint32_t LoRaWANManager::joinRetryCount() const { return joinRetryCount_; }
uint8_t LoRaWANManager::currentDataRate() const { return currentDataRate_; }
String LoRaWANManager::lastError() const { return lastError_; }

void LoRaWANManager::setRegionalProfile(RegionalProfile rp) {
  if (rp > RegionalProfile::AS923_4 || rp == region_) return;
  region_ = rp;
  if (node_ && state_ != LoRaWANState::Joined &&
      state_ != LoRaWANState::Joining) {
    (void)recreateNode();
  }
  updateState();
}

RegionalProfile LoRaWANManager::regionalProfile() const {
  return region_;
}

void LoRaWANManager::captureDownlink(const uint8_t* data, size_t len,
                                      const LoRaWANEvent_t* event) {
  if (!data || len == 0 || len > Config::LORAWAN_MAX_DOWNLINK || !downlinkQueue_)
    return;
  Downlink item{};
  item.fPort = event ? event->fPort : 0;
  item.len = static_cast<uint8_t>(min<size_t>(len, sizeof(item.data)));
  memcpy(item.data, data, item.len);
  if (xQueueSend(downlinkQueue_, &item, 0) == pdPASS) {
    ++downlinkCount_;
  } else {
    lastError_ = "LoRaWAN downlink queue full";
  }
}

bool LoRaWANManager::performUplink(uint8_t fPort, const uint8_t* data,
                                   size_t len, bool confirmed) {
  if (!node_ || state_ != LoRaWANState::Joined) return false;
  uint8_t down[Config::LORAWAN_MAX_DOWNLINK] = {};
  size_t downLen = sizeof(down);
  LoRaWANEvent_t eventUp{};
  LoRaWANEvent_t eventDown{};
  const int16_t st = node_->sendReceive(data, len, fPort, down, &downLen,
                                        confirmed, &eventUp, &eventDown);
  currentDataRate_ = eventUp.datarate;
  lastRssi_ = p2p_.radioRssi();
  lastSnr_ = p2p_.radioSnr();
  if (st < RADIOLIB_ERR_NONE) {
    setError("Uplink failed: " + String(st));
    return false;
  }
  // RadioLib updates FCntUp/FCntDown in its session buffer during sendReceive().
  // Persist it before allowing another uplink so a reboot cannot reuse a frame counter.
  if (!saveSession()) {
    setError("LoRaWAN session persistence failed");
    connectRequested_ = false;
    return false;
  }
  ++uplinkCount_;
  if (downLen) captureDownlink(down, downLen, &eventDown);
  updateState();
  return true;
}

void LoRaWANManager::servicePeriodicTelemetry() {
  RuntimeConfig config{}; if (!configSnapshot(config)) return;
  if (state_ != LoRaWANState::Joined || config.lorawanUplinkPeriodSec == 0)
    return;
  const uint32_t periodMs =
      static_cast<uint32_t>(config.lorawanUplinkPeriodSec) * 1000UL;
  if (millis() - lastUplinkMs_ < periodMs) return;

  const String payload = makeLoRaWANUplinkJson();
  if (payload.isEmpty() || payload.length() > Config::LORAWAN_MAX_PAYLOAD)
    return;
  if (manualUplinkPending_) return;
  memcpy(manualPayload_, payload.c_str(), payload.length());
  manualLen_ = static_cast<uint8_t>(payload.length());
  manualFPort_ = config.lorawanFPort;
  manualConfirmed_ = false;
  manualUplinkPending_ = true;
  lastUplinkMs_ = millis();
}

void LoRaWANManager::task() {
  if (!ready_ || !mutex_) return;

  if (xSemaphoreTake(mutex_, 0) != pdTRUE) return;
  const bool doDisconnect = disconnectRequested_;
  disconnectRequested_ = false;
  const bool doConnect = connectRequested_ &&
      (state_ == LoRaWANState::Joining || state_ == LoRaWANState::Rejoining) &&
      (millis() - lastJoinAttemptMs_ >= retryDelayMs_ || joinRetryCount_ == 0);
  const uint8_t mode = requestedMode_;
  xSemaphoreGive(mutex_);

  if (doDisconnect) {
    RadioArbiterGuard guard(radioArbiter, RadioOwner::LoRaWAN, 0);
    if (!guard.ok()) return;
    p2p_.suspendForLoRaWAN();
    if (node_) node_->clearSession();
    (void)p2p_.resumeFromLoRaWAN();
    state_ = LoRaWANState::Disconnected;
    updateState();
    return;
  }

  if (doConnect) {
    RadioArbiterGuard guard(radioArbiter, RadioOwner::LoRaWAN, 0);
    if (!guard.ok()) return;

    p2p_.suspendForLoRaWAN();
    lastJoinAttemptMs_ = millis();
    ++joinRetryCount_;
    state_ = (joinRetryCount_ == 1) ? LoRaWANState::Joining : LoRaWANState::Rejoining;
    updateState();

    if (startActivation(mode)) {
      connectRequested_ = false;
      retryDelayMs_ = Config::LORAWAN_JOIN_RETRY_MIN_MS;
    } else if (!connectRequested_) {
      // A persistence failure is fail-closed: retrying could reuse a DevNonce
      // or LoRaWAN frame counter that was not durably committed.
      (void)p2p_.resumeFromLoRaWAN();
      updateState();
      return;
    } else {
      state_ = LoRaWANState::Rejoining;
      retryDelayMs_ = min<uint32_t>(
          Config::LORAWAN_JOIN_RETRY_MAX_MS,
          max<uint32_t>(Config::LORAWAN_JOIN_RETRY_MIN_MS,
                        retryDelayMs_ * Config::LORAWAN_JOIN_BACKOFF_MULT));
      updateState();
    }

    if (state_ != LoRaWANState::Joined) {
      (void)p2p_.resumeFromLoRaWAN();
    }
    return;
  }

  if (state_ != LoRaWANState::Joined) return;

  RadioArbiterGuard guard(radioArbiter, RadioOwner::LoRaWAN, 0);
  if (!guard.ok()) return;

  p2p_.suspendForLoRaWAN();
  servicePeriodicTelemetry();

  if (manualUplinkPending_) {
    uint8_t payload[Config::LORAWAN_MAX_PAYLOAD] = {};
    uint8_t len = 0;
    uint8_t fPort = 0;
    bool confirmed = false;
    if (xSemaphoreTake(mutex_, 0) == pdTRUE) {
      len = manualLen_;
      fPort = manualFPort_;
      confirmed = manualConfirmed_;
      memcpy(payload, manualPayload_, len);
      manualUplinkPending_ = false;
      xSemaphoreGive(mutex_);
    }
    if (len) (void)performUplink(fPort, payload, len, confirmed);
  }

  updateState();
  // Keep the SX1262 exclusively in LoRaWAN mode until disconnect or failure.
}
