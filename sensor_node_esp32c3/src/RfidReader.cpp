#include "RfidReader.h"
#include "ProfileConfig.h"

#include <SPI.h>
#include <MFRC522.h>
#include <cstring>

namespace {

MFRC522* gReader = nullptr;

// Returns true if the supplied UID is byte-identical to the cached one.
bool uidMatches(const uint8_t* a, uint8_t aLen, const uint8_t* b, uint8_t bLen) {
  if (aLen != bLen) return false;
  return std::memcmp(a, b, aLen) == 0;
}

}  // namespace

RfidReader::~RfidReader() {
  if (gReader) {
    delete gReader;
    gReader = nullptr;
  }
  ready_ = false;
}

bool RfidReader::begin() {
  // SPI bus is initialized with the pins locked in ProfileConfig.h. The
  // MFRC522 is the only SPI device in RFID; Profile 2/3 drivers (ADXL355)
  // will share this bus in later steps with their own CS pins.
  SPI.begin(ProfileConfig::RFID_SCK_PIN,
            ProfileConfig::RFID_MISO_PIN,
            ProfileConfig::RFID_MOSI_PIN,
            ProfileConfig::RFID_CS_PIN);

  if (!gReader) gReader = new MFRC522(ProfileConfig::RFID_CS_PIN,
                                      ProfileConfig::RFID_RST_PIN);
  if (!gReader) return false;

  gReader->PCD_Init();
  delay(4);  // MFRC522 datasheet: allow the antenna to settle after reset.

  const uint8_t version = gReader->PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    // 0x00 = MISO stuck low; 0xFF = MISO stuck high. Both indicate that the
    // reader is absent or miswired. Non-fatal: caller continues without RFID.
    Serial.printf("WARN: MFRC522 not detected (VersionReg=0x%02X)\n",
                  static_cast<unsigned>(version));
    ready_ = false;
    return false;
  }

  ready_ = true;
  Serial.printf("RFID: MFRC522 ready (VersionReg=0x%02X)\n",
                static_cast<unsigned>(version));
  return true;
}

uint8_t RfidReader::lastUid(uint8_t* out, uint8_t outCapacity) const {
  if (!out || outCapacity < lastUidLength_) return 0;
  std::memcpy(out, lastUid_, lastUidLength_);
  return lastUidLength_;
}

void RfidReader::task() {
  if (!ready_ || !gReader) return;

  const uint32_t now = millis();
  if (now - lastPollMs_ < ProfileConfig::RFID_POLL_INTERVAL_MS) return;
  lastPollMs_ = now;

  const bool present = gReader->PICC_IsNewCardPresent() &&
                       gReader->PICC_ReadCardSerial();

  if (present) {
    const uint8_t* uid = gReader->uid.uidByte;
    const uint8_t uidLen =
        static_cast<uint8_t>(gReader->uid.size > ProfileConfig::RFID_MAX_UID_BYTES
                                 ? ProfileConfig::RFID_MAX_UID_BYTES
                                 : gReader->uid.size);

    const bool isNewUid = !uidPresent_ ||
                          !uidMatches(uid, uidLen, lastUid_, lastUidLength_);

    // Cache the current UID regardless of novelty so uidPresent_ tracks the
    // physical tag in the field.
    std::memcpy(lastUid_, uid, uidLen);
    lastUidLength_ = uidLen;
    lastUidSeenMs_ = now;
    uidPresent_ = true;

    if (isNewUid && tagCallback_) {
      tagCallback_(uid, uidLen);
    }

    // MFRC522 requires the transaction to be halted before the next poll.
    gReader->PICC_HaltA();
    gReader->PCD_StopCrypto1();
    return;
  }

  // No tag currently in the field. Forget the cached UID after a grace window
  // so that re-entering the same tag re-fires the callback.
  if (uidPresent_ && now - lastUidSeenMs_ >= ProfileConfig::RFID_UID_FORGET_MS) {
    uidPresent_ = false;
    lastUidLength_ = 0;
    std::memset(lastUid_, 0, sizeof(lastUid_));
  }
}
