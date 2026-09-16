#include "RfDetector.h"
#include "BoardConfig.h"
#include <math.h>

namespace {
float clampDbm(float value) {
  return constrain(value, Config::MAX2016_MIN_DBM, Config::MAX2016_MAX_DBM);
}
}

bool RfDetector::begin() {
  healthy_ = false;
  haveSample_ = false;
  forwardDbm_ = Config::MAX2016_MIN_DBM;
  reflectedDbm_ = Config::MAX2016_MIN_DBM;
  vswr_ = Config::MAX2016_VSWR_MAX;

  if (Board::MAX2016_OUT_FWD < 0 || Board::MAX2016_OUT_REF < 0 ||
      Board::MAX2016_OUT_FWD == Board::MAX2016_OUT_REF)
    return false;

  pinMode(Board::MAX2016_OUT_FWD, INPUT);
  pinMode(Board::MAX2016_OUT_REF, INPUT);
  analogSetPinAttenuation(Board::MAX2016_OUT_FWD, Config::MAX2016_ADC_ATTENUATION);
  analogSetPinAttenuation(Board::MAX2016_OUT_REF, Config::MAX2016_ADC_ATTENUATION);

  // A MAX2016 RSSI output in detector mode is nominally ~0.5..1.8 V. A zero
  // reading is therefore a useful indication of an unconnected/failed ADC path.
  const uint32_t fwd = analogReadMilliVolts(Board::MAX2016_OUT_FWD);
  const uint32_t ref = analogReadMilliVolts(Board::MAX2016_OUT_REF);
  healthy_ = fwd > 0 && ref > 0;
  return healthy_;
}

float RfDetector::voltageToDbm(uint32_t millivolts) {
  const float mv = static_cast<float>(millivolts);
  return clampDbm((mv / Config::MAX2016_SLOPE_MV_PER_DB) +
                  Config::MAX2016_INTERCEPT_DBM);
}

float RfDetector::returnLossToVswr(float returnLossDb) {
  if (!isfinite(returnLossDb) || returnLossDb <= 0.0f) return Config::MAX2016_VSWR_MAX;
  const float gamma = powf(10.0f, -returnLossDb / 20.0f);
  if (gamma >= 0.999f) return Config::MAX2016_VSWR_MAX;
  return min(Config::MAX2016_VSWR_MAX, (1.0f + gamma) / (1.0f - gamma));
}

bool RfDetector::read(float& forwardDbm, float& reflectedDbm, float& vswr) {
  if (!healthy_) return false;
  if (Config::MAX2016_READ_DELAY_US) delayMicroseconds(Config::MAX2016_READ_DELAY_US);

  const uint32_t fwdMv = analogReadMilliVolts(Board::MAX2016_OUT_FWD);
  const uint32_t refMv = analogReadMilliVolts(Board::MAX2016_OUT_REF);
  if (fwdMv == 0 || refMv == 0) {
    healthy_ = false;
    return false;
  }

  const float fwd = voltageToDbm(fwdMv);
  const float ref = voltageToDbm(refMv);
  const float rl = fwd - ref;
  const float rawVswr = returnLossToVswr(rl);
  if (!haveSample_) {
    forwardDbm_ = fwd;
    reflectedDbm_ = ref;
    vswr_ = rawVswr;
    haveSample_ = true;
  } else {
    const float a = constrain(Config::MAX2016_EMA_ALPHA, 0.01f, 1.0f);
    forwardDbm_ += a * (fwd - forwardDbm_);
    reflectedDbm_ += a * (ref - reflectedDbm_);
    vswr_ += a * (rawVswr - vswr_);
  }
  forwardDbm = forwardDbm_;
  reflectedDbm = reflectedDbm_;
  vswr = vswr_;
  return true;
}
