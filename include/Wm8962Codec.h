#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "Config.h"

class Wm8962Codec {
public:
  bool begin(TwoWire& wire, uint8_t address);
  bool configureI2sMaster(uint32_t sampleRate, uint8_t bitsPerSample);
  bool routeInput(uint8_t source);
  bool setVolumePercent(uint8_t percent);
  bool setMuted(bool muted);
  bool setClassDConfig(bool enabled, uint8_t boostLevel);
  bool setLoopback(bool enabled);

private:
  TwoWire* wire_ = nullptr;
  uint8_t address_ = 0;
  bool classDEnabled_ = Config::CLASS_D_ENABLED;
  uint8_t classDBoostLevel_ = Config::CLASS_D_BOOST_LEVEL;

  bool writeReg(uint16_t reg, uint16_t value);
  bool readReg(uint16_t reg, uint16_t& value);
  bool updateReg(uint16_t reg, uint16_t mask, uint16_t value);
  bool configureClock44k1();
  bool configureAnaloguePath();
  bool configureClassDSpeaker();
  bool runHeadphonePowerUp();
  bool runInputDcServo();
  bool waitForBits(uint16_t reg, uint16_t mask, uint16_t expected,
                   uint32_t timeoutMs);
};
