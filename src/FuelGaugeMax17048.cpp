#include "FuelGaugeMax17048.h"
#include "AppState.h"
#include "Config.h"
#include <Wire.h>

namespace {
constexpr uint8_t ADDRESS = 0x36;
constexpr uint8_t REG_VCELL = 0x02;
constexpr uint8_t REG_SOC = 0x04;

bool read16(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(ADDRESS), 2, true) != 2) return false;
  const int hi = Wire.read();
  const int lo = Wire.read();
  if (hi < 0 || lo < 0) return false;
  value = static_cast<uint16_t>((hi << 8) | lo);
  return true;
}
}

bool FuelGaugeMax17048::begin() {
  available_ = false;
  voltage_ = NAN;
  percent_ = -1;
  lastPollMs_ = 0;
  if (!gI2cMutex) return false;
  Wire.setTimeOut(Config::I2C_TIMEOUT_MS);
  I2cLock lock(pdMS_TO_TICKS(Config::I2C_TIMEOUT_MS));
  if (!lock.ok()) return false;
  Wire.beginTransmission(ADDRESS);
  available_ = Wire.endTransmission() == 0;
  return available_;
}

void FuelGaugeMax17048::task() {
  const uint32_t now = millis();
  if (!available_ || (lastPollMs_ != 0U && now - lastPollMs_ < Config::BATTERY_GAUGE_POLL_MS)) return;
  lastPollMs_ = now;

  I2cLock lock(pdMS_TO_TICKS(Config::I2C_TIMEOUT_MS));
  if (!lock.ok()) return;
  uint16_t vcell = 0, socRaw = 0;
  if (!read16(REG_VCELL, vcell) || !read16(REG_SOC, socRaw)) {
    available_ = false;
    voltage_ = NAN;
    percent_ = -1;
    return;
  }
  // VCELL uses the upper 12 bits with 78.125 uV/LSB; SOC is 8.8 fixed-point.
  voltage_ = static_cast<float>(vcell >> 4) * 0.078125f / 1000.0f;
  const float soc = static_cast<float>(socRaw) / 256.0f;
  if (!isfinite(voltage_) || voltage_ < 2.0f || voltage_ > 5.0f ||
      !isfinite(soc) || soc < 0.0f || soc > 100.0f) {
    available_ = false;
    voltage_ = NAN;
    percent_ = -1;
    return;
  }
  percent_ = static_cast<int8_t>(constrain(soc, 0.0f, 100.0f));
}
