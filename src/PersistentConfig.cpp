#include "PersistentConfig.h"
#include "Config.h"
#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

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
    Config::WEB_PASSWORD,
    "",
    ""};

namespace {
constexpr char NVS_NS[] = "fieldradio";
constexpr uint32_t CONFIG_VERSION = Config::CONFIG_VERSION;
constexpr size_t PASSWORD_SALT_BYTES = 16;
constexpr uint32_t PASSWORD_HASH_ROUNDS = 10000;
constexpr float MIN_FREQ_MHZ = 920.0f;
constexpr float MAX_FREQ_MHZ = 923.0f;
constexpr float MIN_BW_KHZ = 7.8f;
constexpr float MAX_BW_KHZ = 250.0f;

String hexEncode(const uint8_t* data, size_t len) {
  const char* d = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += d[data[i] >> 4];
    out += d[data[i] & 0x0F];
  }
  return out;
}

bool hexDecode(const String& in, uint8_t* out, size_t len) {
  if (!out || in.length() != len * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = nibble(in[i * 2]);
    const int lo = nibble(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool passwordHash(const String& password, const uint8_t* salt,
                  uint8_t out[32]) {
  if (!salt || !out || password.isEmpty() || password.length() > 63) return false;
  uint8_t state[32] = {};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0 &&
            mbedtls_sha256_update(&ctx, salt, PASSWORD_SALT_BYTES) == 0 &&
            mbedtls_sha256_update(&ctx,
                                  reinterpret_cast<const uint8_t*>(password.c_str()),
                                  password.length()) == 0 &&
            mbedtls_sha256_finish(&ctx, state) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;

  for (uint32_t i = 1; i < PASSWORD_HASH_ROUNDS; ++i) {
    mbedtls_sha256_init(&ctx);
    ok = mbedtls_sha256_starts(&ctx, 0) == 0 &&
         mbedtls_sha256_update(&ctx, state, sizeof(state)) == 0 &&
         mbedtls_sha256_update(&ctx, salt, PASSWORD_SALT_BYTES) == 0 &&
         mbedtls_sha256_finish(&ctx, state) == 0;
    mbedtls_sha256_free(&ctx);
    if (!ok) return false;
  }
  memcpy(out, state, sizeof(state));
  return true;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

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
  const String webPasswordValue = prefs.getString("webpass", "");
  const String webPasswordSaltValue = prefs.getString("websalt", "");
  const String webPasswordHashValue = prefs.getString("webph", "");
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
  candidate.webPasswordSaltHex = webPasswordSaltValue;
  candidate.webPasswordHashHex = webPasswordHashValue;

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
  uint8_t passwordSaltCheck[PASSWORD_SALT_BYTES] = {};
  if (hexDecode(candidate.webPasswordSaltHex, passwordSaltCheck, sizeof(passwordSaltCheck)) &&
      candidate.webPasswordHashHex.length() == 64) {
    webPasswordSaltHex = candidate.webPasswordSaltHex;
    webPasswordHashHex = candidate.webPasswordHashHex;
    if (candidate.webPassword.isEmpty()) webPassword.clear();
  }

  // One-time migration: remove the legacy plaintext web password from NVS.
  // Runtime memory may temporarily contain the password because HTTP Basic
  // authentication still needs the cleartext value until the next reboot.
  if (!webPassword.isEmpty()) {
    (void)save();
  } else if (version != CONFIG_VERSION && webPasswordConfigured()) {
    (void)save();
  }
}

bool RuntimeConfig::save() const {
  if (!validRadio() || volume > 100 || audioRecordSource > Config::AUDIO_SOURCE_USB ||
      !isfinite(batteryCalibration) ||
      batteryCalibration < 0.5f || batteryCalibration > 1.5f ||
!validCallsign(callsign) || !validHexKey(loraKeyHex) || apSsid.isEmpty() || apSsid.length() > 32 ||
      apPassword.length() < 8 || apPassword.length() > 63 ||
      webUser.isEmpty() || webUser.length() > 32 ||
      !webPasswordConfigured())
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
            prefs.putString("webuser", webUser) > 0;
  if (ok) {
    uint8_t salt[PASSWORD_SALT_BYTES] = {};
    uint8_t hash[32] = {};
    bool haveCredential = false;
    if (!webPassword.isEmpty()) {
      for (size_t i = 0; i < sizeof(salt); i += 4) {
        const uint32_t r = esp_random();
        memcpy(salt + i, &r, min<size_t>(4, sizeof(salt) - i));
      }
      haveCredential = passwordHash(webPassword, salt, hash);
      if (haveCredential) {
        ok = prefs.putString("websalt", hexEncode(salt, sizeof(salt))) > 0 &&
             prefs.putString("webph", hexEncode(hash, sizeof(hash))) > 0;
      }
    } else {
      uint8_t existingSalt[PASSWORD_SALT_BYTES] = {};
      haveCredential = hexDecode(webPasswordSaltHex, existingSalt, sizeof(existingSalt)) &&
                       webPasswordHashHex.length() == 64;
      ok = haveCredential &&
           prefs.putString("websalt", webPasswordSaltHex) > 0 &&
           prefs.putString("webph", webPasswordHashHex) > 0;
    }
    // Deliberately remove the legacy plaintext key on every successful save.
    if (ok) (void)prefs.remove("webpass");
  }
  prefs.end();
  return ok;
}

bool RuntimeConfig::webPasswordConfigured() const {
  if (webPassword.length() >= 8 && webPassword.length() <= 63) return true;
  uint8_t salt[PASSWORD_SALT_BYTES] = {};
  uint8_t hash[32] = {};
  return hexDecode(webPasswordSaltHex, salt, sizeof(salt)) &&
         webPasswordHashHex.length() == 64 &&
         hexDecode(webPasswordHashHex, hash, sizeof(hash));
}

bool RuntimeConfig::verifyWebPassword(const String& password) const {
  if (password.length() < 8 || password.length() > 63) return false;
  if (!webPassword.isEmpty()) {
    if (password.length() != webPassword.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < password.length(); ++i)
      diff |= static_cast<uint8_t>(password[i] ^ webPassword[i]);
    return diff == 0;
  }
  uint8_t salt[PASSWORD_SALT_BYTES] = {};
  uint8_t expected[32] = {};
  uint8_t got[32] = {};
  if (!hexDecode(webPasswordSaltHex, salt, sizeof(salt)) ||
      !hexDecode(webPasswordHashHex, expected, sizeof(expected)) ||
      !passwordHash(password, salt, got))
    return false;
  return constantTimeEqual(expected, got, sizeof(expected));
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
