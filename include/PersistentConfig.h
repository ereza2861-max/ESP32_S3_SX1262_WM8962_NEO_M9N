#pragma once
#include <Arduino.h>
#include "Config.h"

struct RuntimeConfig {
  float loraFreqMHz;
  float loraBwKHz;
  uint8_t loraSf;
  uint8_t loraCr;
  uint8_t loraSyncWord;
  int8_t loraPowerDbm;
  uint8_t volume;
  uint8_t audioRecordSource;
  float batteryCalibration;
  String callsign;
  String loraKeyHex;
  String apSsid;
  String apPassword;
  String webUser;
  String webPassword;                 // runtime-only plaintext; never persisted
  String webPasswordSaltHex;           // persisted credential salt
  String webPasswordHashHex;           // persisted credential hash
  uint8_t audioRecordQuality = 2;
  bool lorawanEnabled = false;
  uint8_t lorawanMode = 0;
  uint8_t lorawanRegion = Config::LORAWAN_REGION_DEFAULT;
  String lorawanDevEui;
  String lorawanJoinEui;
  String lorawanAppKey;
  String lorawanNwkSKey;
  String lorawanAppSKey;
  uint8_t lorawanDevAddr[4] = {};
  uint8_t lorawanFPort = Config::LORAWAN_DEFAULT_FPORT;
  uint16_t lorawanUplinkPeriodSec = Config::LORAWAN_UPLINK_PERIOD_SEC_DEFAULT;
  bool blePairingEnabled = Config::BLE_PAIRING_ENABLED_VALUE;
  bool mqttEnabled = true;
  uint32_t wakePeriodSec = Config::GNSS_TIME_SYNC_PERIOD_MS / 1000UL;
  bool classDEnabled = false;
  uint8_t classDBoostLevel = Config::CLASS_D_BOOST_LEVEL;
  // Safety-critical power thresholds/timers are runtime-configurable, but bounded
  // by validation in RuntimeConfig::load/save before they reach the power task.
  bool deepSleepEnabled = Config::DEEP_SLEEP_ENABLED;
  uint32_t deepSleepIdleMs = Config::DEEP_SLEEP_IDLE_MS;
  uint32_t deepSleepWakeGraceMs = Config::DEEP_SLEEP_WAKE_GRACE_MS;
  uint32_t criticalShutdownDelayMs = Config::CRITICAL_SHUTDOWN_DELAY_MS;
  float batteryLowThreshold = Config::BATTERY_LOW_THRESHOLD;
  float batteryCriticalThreshold = Config::BATTERY_CRITICAL;

  void load();
  bool migrate();
  bool save() const;
  bool setRadio(float freqMHz, float bwKHz, uint8_t sf, uint8_t cr,
                uint8_t syncWord, int8_t powerDbm);
  bool validRadio() const;
  bool validLoRaWAN() const;
  bool webPasswordConfigured() const;
  bool verifyWebPassword(const String& password) const;
};

extern RuntimeConfig gConfig;
