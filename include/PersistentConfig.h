#pragma once
#include <Arduino.h>

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

  void load();
  bool migrate();
  bool save() const;
  bool setRadio(float freqMHz, float bwKHz, uint8_t sf, uint8_t cr,
                uint8_t syncWord, int8_t powerDbm);
  bool validRadio() const;
  bool webPasswordConfigured() const;
  bool verifyWebPassword(const String& password) const;
};

extern RuntimeConfig gConfig;
