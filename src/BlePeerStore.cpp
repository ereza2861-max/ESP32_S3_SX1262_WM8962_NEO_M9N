#include "BlePeerStore.h"
#include <cstring>

#ifdef ARDUINO
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#else
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace BlePeerStore {
namespace {
constexpr char MASTER_LABEL[] = "FieldRadio-BLE-Peer-v2";
constexpr char IV_LABEL[] = "iv";

bool decodeLoraKey(const char* text, uint8_t out[16]) {
  if (!text || std::strlen(text) != 32) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < 16; ++i) {
    const int hi = nibble(text[i * 2]);
    const int lo = nibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool sha256(const uint8_t* data1, size_t len1,
            const uint8_t* data2, size_t len2, uint8_t out[32]) {
#ifdef ARDUINO
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  const bool ok = mbedtls_sha256_starts(&ctx, 0) == 0 &&
                  mbedtls_sha256_update(&ctx, data1, len1) == 0 &&
                  mbedtls_sha256_update(&ctx, data2, len2) == 0 &&
                  mbedtls_sha256_finish(&ctx, out) == 0;
  mbedtls_sha256_free(&ctx);
  return ok;
#else
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                  EVP_DigestUpdate(ctx, data1, len1) == 1 &&
                  EVP_DigestUpdate(ctx, data2, len2) == 1;
  unsigned int outLen = 0;
  const bool finished = ok && EVP_DigestFinal_ex(ctx, out, &outLen) == 1 && outLen == 32;
  EVP_MD_CTX_free(ctx);
  return finished;
#endif
}

bool hmacSha256(const uint8_t key[16], const uint8_t* data, size_t len,
                uint8_t out[32]) {
#ifdef ARDUINO
  const mbedtls_md_info_t* md =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md && mbedtls_md_hmac(md, key, 16, data, len, out, 32) == 0;
#else
  unsigned int outLen = 0;
  return HMAC(EVP_sha256(), key, 16, data, len, out, &outLen) != nullptr &&
         outLen == 32;
#endif
}

bool aesCtr(const uint8_t key[16], uint8_t iv[16],
            const uint8_t* input, uint8_t* output, size_t len) {
#ifdef ARDUINO
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  size_t ncOff = 0;
  uint8_t stream[16] = {};
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
      mbedtls_aes_crypt_ctr(&aes, len, &ncOff, iv, stream, input, output) == 0;
  mbedtls_aes_free(&aes);
  return ok;
#else
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  int outLen = 0;
  int finalLen = 0;
  const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) == 1 &&
                  EVP_EncryptUpdate(ctx, output, &outLen, input, static_cast<int>(len)) == 1 &&
                  EVP_EncryptFinal_ex(ctx, output + outLen, &finalLen) == 1 &&
                  static_cast<size_t>(outLen + finalLen) == len;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
#endif
}

bool aesEcb(const uint8_t key[16], const uint8_t input[16], uint8_t output[16]) {
#ifdef ARDUINO
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
                  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output) == 0;
  mbedtls_aes_free(&aes);
  return ok;
#else
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  int outLen = 0;
  int finalLen = 0;
  const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
                  EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
                  EVP_EncryptUpdate(ctx, output, &outLen, input, 16) == 1 &&
                  EVP_EncryptFinal_ex(ctx, output + outLen, &finalLen) == 1 &&
                  outLen + finalLen == 16;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
#endif
}

bool cryptSensitive(PeerRecordV2& record, const uint8_t key[16]) {
  uint8_t identity[sizeof(record.identity)] = {};
  std::memcpy(identity, &record.identity, sizeof(identity));
  uint8_t digest[32] = {};
  if (!sha256(identity, sizeof(identity),
              reinterpret_cast<const uint8_t*>(IV_LABEL), sizeof(IV_LABEL) - 1,
              digest)) return false;

  uint8_t iv[16] = {};
  std::memcpy(iv, digest, sizeof(iv));
  uint8_t buffer[sizeof(record.passkey) + sizeof(record.lastRpa)] = {};
  std::memcpy(buffer, record.passkey, sizeof(record.passkey));
  std::memcpy(buffer + sizeof(record.passkey), &record.lastRpa, sizeof(record.lastRpa));
  const bool ok = aesCtr(key, iv, buffer, buffer, sizeof(buffer));
  if (!ok) return false;
  std::memcpy(record.passkey, buffer, sizeof(record.passkey));
  std::memcpy(&record.lastRpa, buffer + sizeof(record.passkey), sizeof(record.lastRpa));
  std::memset(buffer, 0, sizeof(buffer));
  return true;
}
}  // namespace

uint32_t legacyCrc(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619UL;
  }
  return h;
}

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xEDB88320U & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U))));
  }
  return crc ^ 0xFFFFFFFFU;
}

bool deriveMasterKey(const char* loraKeyHex, uint8_t out[16]) {
  if (!out) return false;
  uint8_t ignored[16] = {};
  if (!decodeLoraKey(loraKeyHex, ignored)) return false;
  uint8_t digest[32] = {};
  if (!sha256(reinterpret_cast<const uint8_t*>(loraKeyHex), std::strlen(loraKeyHex),
              reinterpret_cast<const uint8_t*>(MASTER_LABEL), sizeof(MASTER_LABEL) - 1,
              digest)) return false;
  std::memcpy(out, digest, 16);
  return true;
}

bool validV1(const PeerRecordV1& legacy) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&legacy);
  return legacy.magic == MAGIC &&
         legacy.passkey >= 100000U && legacy.passkey <= 999999U &&
         legacy.crc == legacyCrc(bytes, offsetof(PeerRecordV1, crc));
}

bool sealV2(PeerRecordV2& record, const char* loraKeyHex) {
  uint8_t master[16] = {};
  if (!deriveMasterKey(loraKeyHex, master) || record.magic != MAGIC ||
      record.version != VERSION) return false;
  if (!cryptSensitive(record, master)) return false;

  std::memset(record.mac, 0, sizeof(record.mac));
  uint8_t fullMac[32] = {};
  if (!hmacSha256(master, reinterpret_cast<const uint8_t*>(&record),
                  offsetof(PeerRecordV2, mac), fullMac)) return false;
  std::memcpy(record.mac, fullMac, MAC_BYTES);
  record.crc32 = crc32(reinterpret_cast<const uint8_t*>(&record),
                        offsetof(PeerRecordV2, crc32));
  return true;
}

bool openV2(PeerRecordV2& record, const char* loraKeyHex) {
  if (record.magic != MAGIC || record.version != VERSION) return false;
  if (record.crc32 != crc32(reinterpret_cast<const uint8_t*>(&record),
                             offsetof(PeerRecordV2, crc32))) return false;

  uint8_t master[16] = {};
  if (!deriveMasterKey(loraKeyHex, master)) return false;
  uint8_t expected[32] = {};
  if (!hmacSha256(master, reinterpret_cast<const uint8_t*>(&record),
                  offsetof(PeerRecordV2, mac), expected)) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < MAC_BYTES; ++i) diff |= expected[i] ^ record.mac[i];
  if (diff != 0) return false;
  if (!cryptSensitive(record, master)) return false;
  const uint32_t key = passkey(record);
  return key >= 100000U && key <= 999999U;
}

bool migrateV1(const PeerRecordV1& legacy, PeerRecordV2& out, const char* loraKeyHex) {
  if (!validV1(legacy)) return false;
  out = {};
  out.magic = MAGIC;
  out.version = VERSION;
  setPasskey(out, legacy.passkey);
  out.identity = legacy.identity;
  out.lastRpa = legacy.lastRpa;
  std::memcpy(out.name, legacy.name, sizeof(out.name));
  out.updatedEpoch = legacy.updatedEpoch;
  return sealV2(out, loraKeyHex);
}

uint32_t passkey(const PeerRecordV2& record) {
  return static_cast<uint32_t>(record.passkey[0]) |
         (static_cast<uint32_t>(record.passkey[1]) << 8U) |
         (static_cast<uint32_t>(record.passkey[2]) << 16U) |
         (static_cast<uint32_t>(record.passkey[3]) << 24U);
}

void setPasskey(PeerRecordV2& record, uint32_t value) {
  record.passkey[0] = static_cast<uint8_t>(value);
  record.passkey[1] = static_cast<uint8_t>(value >> 8U);
  record.passkey[2] = static_cast<uint8_t>(value >> 16U);
  record.passkey[3] = static_cast<uint8_t>(value >> 24U);
}

bool matchesRpa(const SensorProtocol::BleAddress& rpa, const uint8_t irk[16]) {
  if (!irk || rpa.type != 1U || (rpa.bytes[5] & 0xC0U) != 0x40U) return false;
  uint8_t block[16] = {};
  block[13] = rpa.bytes[5];
  block[14] = rpa.bytes[4];
  block[15] = rpa.bytes[3];
  uint8_t cipher[16] = {};
  if (!aesEcb(irk, block, cipher)) return false;
  return cipher[13] == rpa.bytes[2] &&
         cipher[14] == rpa.bytes[1] &&
         cipher[15] == rpa.bytes[0];
}

}  // namespace BlePeerStore
