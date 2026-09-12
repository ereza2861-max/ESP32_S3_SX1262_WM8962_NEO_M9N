#include "PersistentConfig.h"
#include "Config.h"
#include <Preferences.h>

RuntimeConfig gConfig{
    Config::LORA_FREQ_MHZ,
    Config::LORA_BW_KHZ,
    Config::LORA_SF,
    Config::LORA_CR,
    Config::LORA_SYNC_WORD,
    Config::LORA_POWER_DBM,
    70,
    Config::AUDIO_SOURCE_WM8962_MIC,
    1.0f,
    Config::LORA_DEFAULT_CALLSIGN,
    Config::LORA_KEY_HEX,
    Config::AP_SSID,
    Config::AP_PASSWORD,
    Config::WEB_USER,
    Config::WEB_PASSWORD};

namespace {
constexpr char NVS_NS[] = "fieldradio";
constexpr uint32_t CONFIG_VERSION = 2;
constexpr float MIN_FREQ_MHZ = 920.0f;
constexpr float MAX_FREQ_MHZ = 923.0f;
constexpr float MIN_BW_KHZ = 7.8f;
constexpr float MAX_BW_KHZ = 250.0f;

bool validCredential(const String& value, size_t maxLen) {
  return !value.isEmpty() && value.length() <= maxLen;
}

bool validHexKey(const String& value) {
  if (value.length() != 32) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool validCallsign(const String& value) {
  if (value.isEmpty() || value.length() > 16) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}
}

bool RuntimeConfig::validRadio() const {
  return isfinite(loraFreqMHz) && loraFreqMHz >= MIN_FREQ_MHZ &&
         loraFreqMHz <= MAX_FREQ_MHZ &&
         isfinite(loraBwKHz) && loraBwKHz >= MIN_BW_KHZ &&
         loraBwKHz <= MAX_BW_KHZ &&
         loraSf >= 5 && loraSf <= 12 &&
         loraCr >= 5 && loraCr <= 8 &&
         loraPowerDbm >= 2 && loraPowerDbm <= 17;
}

void RuntimeConfig::load() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return;

  const uint32_t version = prefs.getUInt("cfgver", 0);
  const float freq = prefs.getFloat("freq", loraFreqMHz);
  const float bw = prefs.getFloat("bw", loraBwKHz);
  const uint8_t sf = prefs.getUChar("sf", loraSf);
  const uint8_t cr = prefs.getUChar("cr", loraCr);
  const uint8_t sw = prefs.getUChar("sync", loraSyncWord);
  const int8_t power = prefs.getChar("power", loraPowerDbm);
  const uint8_t savedVolume = prefs.getUChar("volume", volume);
  const uint8_t savedAudioSource = prefs.getUChar("audsrc", audioRecordSource);
  const float battery = prefs.getFloat("batcal", batteryCalibration);
  const String callsignValue = prefs.getString("callsign", callsign);
  const String loraKeyValue = prefs.getString("lorakey", loraKeyHex);
  const String apSsidValue = prefs.getString("apssid", apSsid);
  const String apPasswordValue = prefs.getString("appass", apPassword);
  const String webUserValue = prefs.getString("webuser", webUser);
  const String webPasswordValue = prefs.getString("webpass", webPassword);
  prefs.end();

  RuntimeConfig candidate = *this;
  candidate.loraFreqMHz = freq;
  candidate.loraBwKHz = bw;
  candidate.loraSf = sf;
  candidate.loraCr = cr;
  candidate.loraSyncWord = sw;
  candidate.loraPowerDbm = power;
  candidate.volume = savedVolume;
  candidate.audioRecordSource = savedAudioSource;
  candidate.batteryCalibration = battery;
  candidate.callsign = callsignValue;
  candidate.loraKeyHex = loraKeyValue;
  candidate.apSsid = apSsidValue;
  candidate.apPassword = apPasswordValue;
  candidate.webUser = webUserValue;
  candidate.webPassword = webPasswordValue;

  if (candidate.validRadio()) {
    loraFreqMHz = candidate.loraFreqMHz;
    loraBwKHz = candidate.loraBwKHz;
    loraSf = candidate.loraSf;
    loraCr = candidate.loraCr;
    loraSyncWord = candidate.loraSyncWord;
    loraPowerDbm = candidate.loraPowerDbm;
  }
  if (candidate.volume <= 100) volume = candidate.volume;
  if (candidate.audioRecordSource <= Config::AUDIO_SOURCE_USB) {
    audioRecordSource = candidate.audioRecordSource;
  }
  if (isfinite(candidate.batteryCalibration) &&
      candidate.batteryCalibration >= 0.5f &&
      candidate.batteryCalibration <= 1.5f)
    batteryCalibration = candidate.batteryCalibration;
  if (validCallsign(candidate.callsign)) callsign = candidate.callsign;
  if (validHexKey(candidate.loraKeyHex)) loraKeyHex = candidate.loraKeyHex;
  if (validCredential(candidate.apSsid, 32)) apSsid = candidate.apSsid;
  if (candidate.apPassword.length() <= 63) apPassword = candidate.apPassword;
  if (validCredential(candidate.webUser, 32)) webUser = candidate.webUser;
  if (candidate.webPassword.length() <= 63) webPassword = candidate.webPassword;

  // Older layouts are read using the same keys, then rewritten with an
  // explicit version on the next successful save. Invalid credentials never
  // replace the compiled/default values.
  (void)version;
}

bool RuntimeConfig::save() const {
  if (!validRadio() || volume > 100 || audioRecordSource > Config::AUDIO_SOURCE_USB ||
      !isfinite(batteryCalibration) ||
      batteryCalibration < 0.5f || batteryCalibration > 1.5f ||
!validCallsign(callsign) || !validHexKey(loraKeyHex) || apSsid.isEmpty() || apSsid.length() > 32 ||
      apPassword.length() > 63 || webUser.isEmpty() || webUser.length() > 32 ||
      webPassword.length() > 63)
    return false;

  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return false;
  bool ok = prefs.putUInt("cfgver", CONFIG_VERSION) > 0 &&
            prefs.putFloat("freq", loraFreqMHz) &&
            prefs.putFloat("bw", loraBwKHz) &&
            prefs.putUChar("sf", loraSf) &&
            prefs.putUChar("cr", loraCr) &&
            prefs.putUChar("sync", loraSyncWord) &&
            prefs.putChar("power", loraPowerDbm) &&
            prefs.putUChar("volume", volume) &&
            prefs.putUChar("audsrc", audioRecordSource) &&
            prefs.putFloat("batcal", batteryCalibration) &&
            prefs.putString("callsign", callsign) > 0 &&
            prefs.putString("lorakey", loraKeyHex) > 0 &&
            prefs.putString("apssid", apSsid) > 0 &&
            prefs.putString("appass", apPassword) > 0 &&
            prefs.putString("webuser", webUser) > 0 &&
            prefs.putString("webpass", webPassword) > 0;
  prefs.end();
  return ok;
}

bool RuntimeConfig::setRadio(float freqMHz, float bwKHz, uint8_t sf,
                             uint8_t cr, uint8_t syncWord, int8_t powerDbm) {
  RuntimeConfig candidate = *this;
  candidate.loraFreqMHz = freqMHz;
  candidate.loraBwKHz = bwKHz;
  candidate.loraSf = sf;
  candidate.loraCr = cr;
  candidate.loraSyncWord = syncWord;
  candidate.loraPowerDbm = powerDbm;
  if (!candidate.validRadio()) return false;
  *this = candidate;
  return true;
}
