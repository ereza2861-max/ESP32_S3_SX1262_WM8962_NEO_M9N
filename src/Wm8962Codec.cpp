#include "Wm8962Codec.h"
#include "BoardConfig.h"
#include "Config.h"

namespace {
constexpr uint16_t R_ADC_DAC_1 = 0x05;
constexpr uint16_t R_ADC_DAC_2 = 0x06;
constexpr uint16_t R_AUDIO_IF_0 = 0x07;
constexpr uint16_t R_AUDIO_IF_1 = 0x09;
constexpr uint16_t R_CLOCKING_2 = 0x08;
constexpr uint16_t R_AUDIO_IF_2 = 0x0E;
constexpr uint16_t R_RESET = 0x0F;
constexpr uint16_t R_ALC_1 = 0x11;
constexpr uint16_t R_ALC_2 = 0x12;
constexpr uint16_t R_ALC_3 = 0x13;
constexpr uint16_t R_NOISE_GATE = 0x14;
constexpr uint16_t R_ADC_L = 0x15;
constexpr uint16_t R_ADC_R = 0x16;
constexpr uint16_t R_PWR_1 = 0x19;
constexpr uint16_t R_PWR_2 = 0x1A;
constexpr uint16_t R_ANTI_POP = 0x1C;
constexpr uint16_t R_ADDITIONAL_CONTROL_3 = 0x1B;
constexpr uint16_t R_INPUT_MIX_1 = 0x1F;
constexpr uint16_t R_INPUT_MIX_L = 0x20;
constexpr uint16_t R_INPUT_MIX_R = 0x21;
constexpr uint16_t R_INPUT_MIX_2 = 0x22;
constexpr uint16_t R_MIXER_ENABLES = 0x63;
constexpr uint16_t R_HP_MIX_L = 0x64;
constexpr uint16_t R_HP_MIX_R = 0x65;
constexpr uint16_t R_INPUT_PGA_L = 0x25;
constexpr uint16_t R_INPUT_PGA_R = 0x26;
constexpr uint16_t R_HP_L = 0x02;
constexpr uint16_t R_HP_R = 0x03;
constexpr uint16_t R_SPKOUT_L = 0x28;
constexpr uint16_t R_SPKOUT_R = 0x29;
constexpr uint16_t R_CLASSD_1 = 0x31;
constexpr uint16_t R_CLASSD_2 = 0x33;
constexpr uint16_t R_DAC_L = 0x0A;
constexpr uint16_t R_DAC_R = 0x0B;
constexpr uint16_t R_FLL_1 = 0x9B;
constexpr uint16_t R_FLL_2 = 0x9C;
constexpr uint16_t R_FLL_3 = 0x9D;
constexpr uint16_t R_FLL_6 = 0xA0;
constexpr uint16_t R_FLL_7 = 0xA1;
constexpr uint16_t R_FLL_8 = 0xA2;
constexpr uint16_t R_WSEQ_CTRL_2 = 0x5A;
constexpr uint16_t R_DC_SERVO_0 = 0x3C;
constexpr uint16_t R_DC_SERVO_6 = 0x42;
constexpr uint16_t WM8962_ID = 0x6243;

constexpr uint16_t PWR1_VMID = 0x0080;
constexpr uint16_t PWR1_BIAS = 0x0040;
constexpr uint16_t PWR1_INL = 0x0020;
constexpr uint16_t PWR1_INR = 0x0010;
constexpr uint16_t PWR1_ADCL = 0x0008;
constexpr uint16_t PWR1_ADCR = 0x0004;
constexpr uint16_t PWR1_MICBIAS = 0x0002;
constexpr uint16_t PWR2_DACL = 0x0100;
constexpr uint16_t PWR2_DACR = 0x0080;
constexpr uint16_t PWR2_HPL = 0x0040;
constexpr uint16_t PWR2_HPR = 0x0020;

constexpr uint16_t AIF0_MASTER = 0x0040;
constexpr uint16_t AIF0_I2S = 0x0002;
constexpr uint16_t AIF0_WL_16 = 0x0000;
constexpr uint16_t CLOCK2_SYSCLK_EN = 0x0020;
constexpr uint16_t CLOCK2_SYSCLK_FLL = 0x0200;
constexpr uint16_t CLOCK2_BCLK_DIV_8 = 0x0006;
constexpr uint16_t AIF2_BCLK_LRCLK_32 = 32;
constexpr uint16_t DAC_MUTE = 0x0008;
constexpr uint16_t DAC_MUTE_RAMP = 0x0010;
constexpr uint16_t CLASSD_DAC_MUTE = 0x0010; // R49/R31 bit 4
constexpr uint16_t SPKOUT_PGA_MUTE = 0x0003;
constexpr uint16_t SPKOUT_EN = 0x00C0;
constexpr uint16_t SPK_MONO = 0x0040;
constexpr uint16_t DAC_UNMUTE_RAMP = 0x0008;
constexpr uint16_t VOL_VU = 0x0100;
constexpr uint16_t HP_VOL_MAX = 0x0079;
constexpr uint16_t DAC_VOL_MAX = 0x00FF;
}

bool Wm8962Codec::writeReg(uint16_t reg, uint16_t value) {
  if (!wire_) return false;
  wire_->beginTransmission(address_);
  wire_->write(static_cast<uint8_t>(reg >> 8));
  wire_->write(static_cast<uint8_t>(reg & 0xFF));
  wire_->write(static_cast<uint8_t>(value >> 8));
  wire_->write(static_cast<uint8_t>(value & 0xFF));
  return wire_->endTransmission() == 0;
}

bool Wm8962Codec::readReg(uint16_t reg, uint16_t& value) {
  if (!wire_) return false;
  wire_->beginTransmission(address_);
  wire_->write(static_cast<uint8_t>(reg >> 8));
  wire_->write(static_cast<uint8_t>(reg & 0xFF));
  if (wire_->endTransmission(false) != 0) return false;
  if (wire_->requestFrom(static_cast<int>(address_), 2) != 2) return false;
  value = static_cast<uint16_t>(wire_->read()) << 8;
  value |= static_cast<uint16_t>(wire_->read());
  return true;
}

bool Wm8962Codec::updateReg(uint16_t reg, uint16_t mask, uint16_t value) {
  uint16_t current = 0;
  if (!readReg(reg, current)) return false;
  current = static_cast<uint16_t>((current & ~mask) | (value & mask));
  return writeReg(reg, current);
}

bool Wm8962Codec::begin(TwoWire& wire, uint8_t address) {
  wire_ = &wire;
  address_ = address;

  uint16_t id = 0;
  if (!readReg(R_RESET, id) || id != WM8962_ID) return false;
  if (!writeReg(R_RESET, WM8962_ID)) return false;
  delay(5);
  return true;
}

bool Wm8962Codec::configureClock44k1() {
  // 24 MHz MCLK -> FLL VCO 90.3168 MHz -> 11.2896 MHz SYSCLK.
  // FLL algorithm values follow the WM8962 reference driver:
  // Fref=12 MHz (MCLK/2), Fout=11.2896 MHz, FLL_OUTDIV=7,
  // FRATIO=1, N=7, THETA=329, LAMBDA=625.
  if (!writeReg(R_FLL_1, 0)) return false;
  if (!writeReg(R_FLL_2, (7U << 3) | 1U)) return false;
  if (!writeReg(R_FLL_3, 0)) return false;
  if (!writeReg(R_FLL_6, 329)) return false;
  if (!writeReg(R_FLL_7, 625)) return false;
  if (!writeReg(R_FLL_8, 7)) return false;
  if (!writeReg(R_FLL_1, 0x0005)) return false;  // MCLK source + fractional + enable
  delay(2);
  if (!writeReg(R_CLOCKING_2,
                CLOCK2_SYSCLK_FLL | CLOCK2_SYSCLK_EN | CLOCK2_BCLK_DIV_8))
    return false;
  return writeReg(R_AUDIO_IF_2, AIF2_BCLK_LRCLK_32);
}

bool Wm8962Codec::waitForBits(uint16_t reg, uint16_t mask, uint16_t expected,
                                uint32_t timeoutMs) {
  const uint32_t started = millis();
  do {
    uint16_t value = 0;
    if (!readReg(reg, value)) return false;
    if ((value & mask) == expected) return true;
    delay(1);
  } while (millis() - started < timeoutMs);
  return false;
}

bool Wm8962Codec::runHeadphonePowerUp() {
  // Cirrus default sequence: VMID/startup bias -> charge pump -> HP PGAs ->
  // HP DC-servo -> output enable. It is specifically designed for pop-free
  // headphone startup and may take up to 93 ms.
  if (!writeReg(R_WSEQ_CTRL_2, 0x0080)) return false;
  if (!waitForBits(R_DC_SERVO_6, 0x0180, 0x0180, 120)) return false;

  // Keep the DAC muted after the hardware sequence has completed. The
  // sequence itself ends by clearing DAC_MUTE, which is unsafe before the
  // I2S stream has known valid samples.
  return updateReg(R_ADC_DAC_1, DAC_MUTE, DAC_MUTE);
}

bool Wm8962Codec::runInputDcServo() {
  // R60: enable + start the left/right input DC servos simultaneously.
  if (!writeReg(R_DC_SERVO_0, 0x00CC)) return false;
  return waitForBits(R_DC_SERVO_6, 0x0600, 0x0600, 120);
}

bool Wm8962Codec::configureClassDSpeaker() {
  if constexpr (!Config::CLASS_D_ENABLED) {
    return true;
  }

  static_assert(Config::CLASS_D_OUTPUT_MODE == Config::ClassDOutputMode::BTL,
                "WM8962 Class-D output is BTL; single-ended is unsupported");
  static_assert(Config::CLASS_D_EXPECTED_SPKVDD_MV == 3300 ||
                    Config::CLASS_D_EXPECTED_SPKVDD_MV == 5000,
                "Class-D SPKVDD contract must be 3.3V or 5.0V");
  static_assert(Config::CLASS_D_BOOST_LEVEL <= 7,
                "WM8962 CLASSD_VOL must be in the 0..7 range");
  static_assert(
      (Config::CLASS_D_MONO && Config::CLASS_D_SPEAKER_IMPEDANCE_OHMS == 4) ||
      (!Config::CLASS_D_MONO && Config::CLASS_D_SPEAKER_IMPEDANCE_OHMS == 8),
      "WM8962 Class-D requires 4 ohm mono or 8 ohm stereo");
  // The speaker PGAs must be powered before the speaker wake sequence.
  if (!updateReg(R_PWR_2, 0x0018, 0x0018)) return false;

  // Start muted. 0x00..0x2F is the hardware mute range; use -68 dB as the
  // first non-mute code so a later unmute cannot jump to full scale.
  if (!writeReg(R_SPKOUT_L, VOL_VU | 0x0030) ||
      !writeReg(R_SPKOUT_R, VOL_VU | 0x0030))
    return false;

  const uint16_t classD2 =
      (Config::CLASS_D_MONO ? SPK_MONO : 0) | Config::CLASS_D_BOOST_LEVEL;
  if (!writeReg(R_CLASSD_2, classD2)) return false;

  // Speaker wake is the datasheet-defined pop-minimising sequence.
  if (!writeReg(R_CLASSD_1, SPKOUT_PGA_MUTE | CLASSD_DAC_MUTE)) return false;
  if (!writeReg(R_WSEQ_CTRL_2, 0x00E8)) return false;
  delay(2);

  // The wake sequence ends by clearing DAC_MUTE. Keep the codec globally
  // muted until a valid I2S stream is running.
  if (!updateReg(R_CLASSD_1, CLASSD_DAC_MUTE | SPKOUT_PGA_MUTE,
                 CLASSD_DAC_MUTE | SPKOUT_PGA_MUTE))
    return false;

  uint16_t status = 0;
  if (!readReg(R_CLASSD_1, status) || (status & SPKOUT_EN) != SPKOUT_EN)
    return false;
  return true;
}

bool Wm8962Codec::configureAnaloguePath() {
  // VMID soft-start and buffered VMID first; use the fast VMID setting only
  // during startup. Normal operation is restored to the 2x50k divider below.
  if (!writeReg(R_ANTI_POP, 0x0018)) return false;
  if (!writeReg(R_PWR_1, PWR1_VMID | PWR1_BIAS |
                         PWR1_INL | PWR1_INR | PWR1_ADCL | PWR1_ADCR))
    return false;

  // 44.1 kHz uses SAMPLE_RATE=000. Keep this explicit.
  if (!writeReg(R_ADDITIONAL_CONTROL_3, 0x0000)) return false;

  // Keep the analogue input mixer active but select exactly one documented
  // source. Input DC-servo is required before ADC capture.
  if (!writeReg(R_ADC_L, 0x00C0)) return false;
  if (!writeReg(R_ADC_R, 0x00C0)) return false;
  if (!writeReg(R_INPUT_MIX_1, 0x0003)) return false;
  if (!writeReg(R_INPUT_MIX_L, 0x0145)) return false;
  if (!writeReg(R_INPUT_MIX_R, 0x0145)) return false;

  if (!writeReg(R_MIXER_ENABLES, 0x0000)) return false;
  // R64/R65=0 deliberately select the direct DACL->HPOUTL and
  // DACR->HPOUTR bypass paths. No speaker mixer is enabled.
  if (!writeReg(R_HP_MIX_L, 0x0000)) return false;
  if (!writeReg(R_HP_MIX_R, 0x0000)) return false;

  // Soft mute and soft unmute are both enabled; use the slower ramp to make
  // start/stop transitions less audible at 44.1 kHz.
  if (!writeReg(R_ADC_DAC_1, DAC_MUTE | DAC_MUTE_RAMP)) return false;
  if (!writeReg(R_ADC_DAC_2, DAC_UNMUTE_RAMP | 0x0004)) return false;
  if (!writeReg(R_ALC_1, 0x0000)) return false;
  if (!writeReg(R_ALC_2, 0x0000)) return false;
  if (!writeReg(R_ALC_3, 0x0000)) return false;
  if (!writeReg(R_NOISE_GATE, 0x0000)) return false;

  return routeInput(Config::AUDIO_SOURCE_WM8962_MIC);
}

bool Wm8962Codec::configureI2sMaster(uint32_t sampleRate, uint8_t bitsPerSample) {
  if (sampleRate != 44100 || bitsPerSample != 16) return false;
  if (!configureClock44k1() || !configureAnaloguePath()) return false;

  // I2S, 16-bit, codec as BCLK/LRCLK master.
  if (!writeReg(R_AUDIO_IF_0, AIF0_MASTER | AIF0_I2S | AIF0_WL_16))
    return false;
  if (!writeReg(R_DAC_L, 0x00C0 | VOL_VU)) return false;
  if (!writeReg(R_DAC_R, 0x00C0 | VOL_VU)) return false;

  // The default headphone power-up sequence is the authoritative pop-free
  // order for VMID, charge pump and DC-servo. Do not manually approximate it.
  if (!runHeadphonePowerUp()) return false;
  if (!configureClassDSpeaker()) return false;

  // Return VMID to the normal 2x50k divider after fast startup.
  if (!updateReg(R_PWR_1, 0x0180, 0x0080)) return false;
  if (!setVolumePercent(70)) return false;
  return setMuted(true);
}

bool Wm8962Codec::routeInput(uint8_t source) {
  // PCB input-net contract:
  //   MIC_L/MIC_R   -> IN1L/IN1R (PGA + MICBIAS)
  //   LINE2_L/R    -> IN2L/IN2R (direct input mixer)
  //   LINE3_L/R    -> IN3L/IN3R (direct input mixer)
  //
  // The source is muted/disconnected before the new path is selected. This
  // avoids a transient mixed-input state during source changes.
  if (source != Config::AUDIO_SOURCE_WM8962_MIC &&
      source != Config::AUDIO_SOURCE_LINEIN2 &&
      source != Config::AUDIO_SOURCE_LINEIN3)
    return false;

  if (!setMuted(true)) return false;
  if (!writeReg(R_INPUT_PGA_L, 0x0000) ||
      !writeReg(R_INPUT_PGA_R, 0x0000))
    return false;

  uint16_t mixer2 = 0x0000;
  bool micBias = false;
  switch (source) {
    case Config::AUDIO_SOURCE_WM8962_MIC:
      mixer2 = 0x0009; // INPGAL/INPGAR -> MIXINL/MIXINR
      micBias = true;
      if (!writeReg(R_INPUT_PGA_L, 0x0018) ||
          !writeReg(R_INPUT_PGA_R, 0x0018))
        return false;
      break;
    case Config::AUDIO_SOURCE_LINEIN2:
      mixer2 = 0x0024; // IN2L/IN2R -> MIXINL/MIXINR
      break;
    case Config::AUDIO_SOURCE_LINEIN3:
      mixer2 = 0x0012; // IN3L/IN3R -> MIXINL/MIXINR
      break;
  }

  if (!writeReg(R_INPUT_MIX_1, 0x0003) ||
      !writeReg(R_INPUT_MIX_2, mixer2))
    return false;

  uint16_t power = PWR1_VMID | PWR1_BIAS |
                   PWR1_INL | PWR1_INR | PWR1_ADCL | PWR1_ADCR;
  if (micBias) power |= PWR1_MICBIAS;
  if (!writeReg(R_PWR_1, power)) return false;

  // Re-run the input DC servo after every source switch. This is required
  // because INL/INR may have a different DC operating point after rerouting.
  if (!runInputDcServo()) return false;

  // Allow the selected ADC path to settle before capture.
  delay(5);
  return true;
}

bool Wm8962Codec::setMuted(bool muted) {
  uint16_t pwr2 = 0;
  if (!readReg(R_PWR_2, pwr2)) return false;
  if (muted) pwr2 |= 0x0003;
  else pwr2 &= static_cast<uint16_t>(~0x0003);
  if (!writeReg(R_PWR_2, pwr2)) return false;

  // HPOUT_VU commits both channel mute bits atomically.
  if (!updateReg(R_HP_L, VOL_VU, VOL_VU)) return false;

  if constexpr (Config::CLASS_D_ENABLED) {
    if (!updateReg(R_CLASSD_1, SPKOUT_PGA_MUTE,
                   muted ? SPKOUT_PGA_MUTE : 0))
      return false;
  }
  return true;
}

bool Wm8962Codec::setVolumePercent(uint8_t percent) {
  percent = constrain(percent, 0, 100);

  // HPOUT 0x00..0x2f is the hardware mute region. Map 0..100% onto the
  // actual -68dB..0dB range instead of spending the lower 40% in mute.
  const uint16_t hp = percent == 0
      ? 0x0000
      : static_cast<uint16_t>(0x0030U +
          ((0x0079U - 0x0030U) * percent) / 100U);
  const uint16_t dac = static_cast<uint16_t>((DAC_VOL_MAX * percent) / 100U);

  if (percent == 0) return setMuted(true);
  if (!writeReg(R_HP_L, VOL_VU | hp)) return false;
  if (!writeReg(R_HP_R, VOL_VU | hp)) return false;
  if (!writeReg(R_DAC_L, VOL_VU | dac)) return false;
  if (!writeReg(R_DAC_R, VOL_VU | dac)) return false;

  if constexpr (Config::CLASS_D_ENABLED) {
    if (!writeReg(R_SPKOUT_L, VOL_VU | hp) ||
        !writeReg(R_SPKOUT_R, VOL_VU | hp))
      return false;
  }
  return setMuted(false);
}

bool Wm8962Codec::setLoopback(bool enabled) {
  return updateReg(R_AUDIO_IF_1, 0x0001, enabled ? 0x0001 : 0);
}
