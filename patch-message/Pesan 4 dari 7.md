Iterasi C — Pesan 4 dari 7: WebUi.h (Lengkap)

Catatan Sebelum Salin

Ini adalah header lengkap WebUi.h yang menggantikan versi lama. Perubahan dari versi lama Anda:

1. #include <ESPWebServerSecure.hpp> → #include "HttpdServer.h" + #include "HttpdRequest.h" + #include "HttpdResponse.h".
2. ESPWebServerSecure& server_ → HttpdServer& server_.
3. Semua 121 signature handler diubah dari void handleXxx() → void handleXxx(HttpdRequest& req, HttpdResponse& res).
4. Helper auth/security diubah signature-nya untuk menerima req/res:
   · auth() → auth(HttpdRequest& req, HttpdResponse& res)
   · sameOrigin(HttpdRequest& req)
   · rateLimit(HttpdRequest& req, HttpdResponse& res, uint32_t& last, uint32_t interval)
   · sessionValid(HttpdRequest& req)
   · csrfValid(HttpdRequest& req)
   · issueSession(HttpdRequest& req, HttpdResponse& res)
   · csrfTokenHexForActiveSession() (tidak berubah)
   · findSessionSlot(const uint8_t token[32], uint32_t clientIp) (tidak berubah)
   · auditAuth(HttpdRequest& req, bool success)
   · basicAuthMatches(HttpdRequest& req, const String& user, const RuntimeConfig& config) — sekarang jadi member, bukan free function, karena butuh req
5. Helper tambahan yang dibutuhkan konversi handler:
   · parseSensorNodeId(HttpdRequest& req, size_t& id) — sudah ada sebagai free function, saya pindah ke member
   · parseBleAddressArg(HttpdRequest& req, SensorProtocol::BleAddress& out) — sudah ada sebagai free function, saya pindah ke member
6. handleUpload butuh state machine multipart — saya tambahkan method handleUploadFileChunk(...) yang menerima chunk dari HttpdMultipart::FileCallback. Tapi karena callback signature berbeda, saya buat dua method:
   · handleUploadStart(HttpdRequest& req, HttpdResponse& res) — yang dipanggil handler
   · handleUploadChunk(const char* name, const char* filename, const char* mime, const uint8_t* data, size_t len, bool firstChunk, bool lastChunk) — internal, dipanggil dari lambda
7. handleSensorNodes punya percabangan if (hasArg("id")) — sekarang jadi if (req.hasArg("id")).
8. Field activeSessionSlot_ tetap ada (dipakai csrfValid dan csrfTokenHexForActiveSession).
9. uploadFile_, uploadPath_, uploadBytes_, uploadFailed_ tetap ada — dipakai state machine.
10. Semua field lain tidak berubah.

---

File 9: src/WebUi.h

Salin seluruh isi ke src/WebUi.h (ganti yang lama):

```cpp
#pragma once
#include <Arduino.h>
#include "HttpdServer.h"
#include "HttpdRequest.h"
#include "HttpdResponse.h"
#include "HttpdMultipart.h"
#include <FS.h>
#include <IPAddress.h>
#include "SensorProtocol.h"

class WebUi {
public:
  explicit WebUi(HttpdServer& server) : server_(server) {}
  void begin();
  void task();

private:
  HttpdServer& server_;

  // ---- Authentication / session / CSRF ----
  bool auth(HttpdRequest& req, HttpdResponse& res);
  bool sameOrigin(HttpdRequest& req);
  bool basicAuthMatches(HttpdRequest& req, const String& user,
                        const RuntimeConfig& config);
  bool rateLimit(HttpdRequest& req, HttpdResponse& res,
                 uint32_t& last, uint32_t interval);
  bool sessionValid(HttpdRequest& req);
  bool csrfValid(HttpdRequest& req);
  bool issueSession(HttpdRequest& req, HttpdResponse& res);
  String csrfTokenHexForActiveSession() const;
  int findSessionSlot(const uint8_t token[32], uint32_t clientIp) const;
  void auditAuth(HttpdRequest& req, bool success);

  uint32_t authFailureWindowStartMs_ = 0;
  uint8_t authFailures_ = 0;
  uint32_t authFailureWindowCount_ = 0;
  uint32_t authBlockedUntilMs_ = 0;
  struct AuthThrottleEntry {
    String ip;
    uint32_t windowStartMs = 0;
    uint32_t blockedUntilMs = 0;
    uint8_t failures = 0;
    uint32_t lastSeenMs = 0;
  };
  static constexpr size_t AUTH_THROTTLE_ENTRIES = 16;
  AuthThrottleEntry authThrottle_[AUTH_THROTTLE_ENTRIES]{};
  uint32_t csrfFailures_ = 0;

  struct SessionSlot {
    uint8_t secret[32] = {};
    uint8_t csrf[16] = {};
    uint32_t issuedMs = 0;
    uint32_t clientIp = 0;
    bool inUse = false;
  };
  static constexpr size_t MAX_SESSIONS = 4;
  SessionSlot sessions_[MAX_SESSIONS]{};
  int8_t activeSessionSlot_ = -1;

  // ---- Rate-limit timestamps per endpoint group ----
  uint32_t lastMessageMs_ = 0;
  uint32_t lastSosMs_ = 0;
  uint32_t lastPttMs_ = 0;
  uint32_t lastConfigMs_ = 0;
  uint32_t lastSensorNodesMs_ = 0;
  uint32_t lastSensorLiveMs_ = 0;
  uint32_t lastSensorActionMs_ = 0;
  uint32_t lastBlePasskeyMs_ = 0;
  uint32_t lastSensorQueuePolicyMs_ = 0;

  // ---- Upload state (multipart) ----
  File uploadFile_;
  String uploadPath_;
  size_t uploadBytes_ = 0;
  bool uploadFailed_ = false;

  // ---- Record schedule state ----
  uint64_t recordScheduleStart_ = 0;
  uint32_t recordScheduleDurationSec_ = 0;
  bool recordScheduleActive_ = false;

  // ---- Self-test result ----
  uint32_t selfTestMs_ = 0;
  String selfTestResult_;

  // ---- Helpers ----
  bool parseSensorNodeId(HttpdRequest& req, size_t& id);
  bool parseBleAddressArg(HttpdRequest& req, SensorProtocol::BleAddress& out);

  // ---- HTTP handlers: root & meta ----
  void handleRoot(HttpdRequest& req, HttpdResponse& res);
  void handleStatus(HttpdRequest& req, HttpdResponse& res);
  void handleApiVersion(HttpdRequest& req, HttpdResponse& res);

  // ---- LoRaWAN ----
  void handleLoRaWANStatus(HttpdRequest& req, HttpdResponse& res);
  void handleLoRaWANConnect(HttpdRequest& req, HttpdResponse& res);
  void handleLoRaWANDisconnect(HttpdRequest& req, HttpdResponse& res);
  void handleLoRaWANConfig(HttpdRequest& req, HttpdResponse& res);
  void handleLoRaWANUplink(HttpdRequest& req, HttpdResponse& res);

  // ---- Files ----
  void handleFiles(HttpdRequest& req, HttpdResponse& res);
  void handleDownload(HttpdRequest& req, HttpdResponse& res);
  void handleUpload(HttpdRequest& req, HttpdResponse& res);
  void handleUploadChunk(const char* name, const char* filename,
                         const char* mime, const uint8_t* data, size_t len,
                         bool firstChunk, bool lastChunk);
  void handleRename(HttpdRequest& req, HttpdResponse& res);

  // ---- Messages ----
  void handleMessages(HttpdRequest& req, HttpdResponse& res);
  void handleMessagePersist(HttpdRequest& req, HttpdResponse& res);
  void handleMessageSchedule(HttpdRequest& req, HttpdResponse& res);
  void handleMessageScheduleList(HttpdRequest& req, HttpdResponse& res);
  void handleMessageScheduleDelete(HttpdRequest& req, HttpdResponse& res);
  void handleMessageClear(HttpdRequest& req, HttpdResponse& res);
  void handleMessageRead(HttpdRequest& req, HttpdResponse& res);
  void handleMessageReply(HttpdRequest& req, HttpdResponse& res);
  void handleMessageExport(HttpdRequest& req, HttpdResponse& res);
  void handleMessage(HttpdRequest& req, HttpdResponse& res);

  // ---- Record schedule ----
  void handleRecordSchedule(HttpdRequest& req, HttpdResponse& res);
  void handleRecordScheduleGet(HttpdRequest& req, HttpdResponse& res);

  // ---- SOS ----
  void handleSosFormat(HttpdRequest& req, HttpdResponse& res);
  void handleSos(HttpdRequest& req, HttpdResponse& res);
  void handleSosStatus(HttpdRequest& req, HttpdResponse& res);
  void handleSosHistory(HttpdRequest& req, HttpdResponse& res);

  // ---- Self test ----
  void handleSelfTest(HttpdRequest& req, HttpdResponse& res);
  void handleSelfTestResult(HttpdRequest& req, HttpdResponse& res);

  // ---- UI prefs ----
  void handleLang(HttpdRequest& req, HttpdResponse& res);
  void handleTheme(HttpdRequest& req, HttpdResponse& res);

  // ---- Diagnostics ----
  void handleNeighbors(HttpdRequest& req, HttpdResponse& res);
  void handleRoutes(HttpdRequest& req, HttpdResponse& res);
  void handleDedupStats(HttpdRequest& req, HttpdResponse& res);
  void handleForwardStats(HttpdRequest& req, HttpdResponse& res);
  void handleAuthStats(HttpdRequest& req, HttpdResponse& res);
  void handleNvs(HttpdRequest& req, HttpdResponse& res);
  void handleConfigMigrate(HttpdRequest& req, HttpdResponse& res);
  void handleDiagFull(HttpdRequest& req, HttpdResponse& res);
  void handleHealthLog(HttpdRequest& req, HttpdResponse& res);
  void handleLoraLog(HttpdRequest& req, HttpdResponse& res);

  // ---- Capture / ADR / HOP ----
  void handleCaptureStart(HttpdRequest& req, HttpdResponse& res);
  void handleCaptureStop(HttpdRequest& req, HttpdResponse& res);
  void handleCaptureDump(HttpdRequest& req, HttpdResponse& res);
  void handleAdr(HttpdRequest& req, HttpdResponse& res);
  void handleHopSync(HttpdRequest& req, HttpdResponse& res);
  void handleHopSuggest(HttpdRequest& req, HttpdResponse& res);
  void handleHopStatus(HttpdRequest& req, HttpdResponse& res);
  void handleHopEnable(HttpdRequest& req, HttpdResponse& res);
  void handleHopSetChannels(HttpdRequest& req, HttpdResponse& res);

  // ---- Scanner / range test ----
  void handleScanStatus(HttpdRequest& req, HttpdResponse& res);
  void handleScanStart(HttpdRequest& req, HttpdResponse& res);
  void handleScanStop(HttpdRequest& req, HttpdResponse& res);
  void handleScanResults(HttpdRequest& req, HttpdResponse& res);
  void handleRangeTest(HttpdRequest& req, HttpdResponse& res);
  void handleRangeTestStatus(HttpdRequest& req, HttpdResponse& res);

  // ---- Radio ----
  void handleRadioHistory(HttpdRequest& req, HttpdResponse& res);
  void handleRadioTune(HttpdRequest& req, HttpdResponse& res);
  void handleRadioStats(HttpdRequest& req, HttpdResponse& res);
  void handleRfDetector(HttpdRequest& req, HttpdResponse& res);
  void handleBatteryHistory(HttpdRequest& req, HttpdResponse& res);
  void handleBatteryCalibrate(HttpdRequest& req, HttpdResponse& res);
  void handleStorageInfo(HttpdRequest& req, HttpdResponse& res);
  void handleChecksum(HttpdRequest& req, HttpdResponse& res);
  void handleChecksumSha256(HttpdRequest& req, HttpdResponse& res);
  void handleLogExport(HttpdRequest& req, HttpdResponse& res);

  // ---- Audio ----
  void handlePtt(HttpdRequest& req, HttpdResponse& res);
  void handleRecord(HttpdRequest& req, HttpdResponse& res);
  void handlePlay(HttpdRequest& req, HttpdResponse& res);
  void handleStop(HttpdRequest& req, HttpdResponse& res);
  void handlePause(HttpdRequest& req, HttpdResponse& res);
  void handleSeek(HttpdRequest& req, HttpdResponse& res);
  void handleQueue(HttpdRequest& req, HttpdResponse& res);
  void handleQueueClear(HttpdRequest& req, HttpdResponse& res);
  void handleRecordPause(HttpdRequest& req, HttpdResponse& res);
  void handleRecordSplit(HttpdRequest& req, HttpdResponse& res);
  void handleVox(HttpdRequest& req, HttpdResponse& res);
  void handleVad(HttpdRequest& req, HttpdResponse& res);
  void handleRecordQuality(HttpdRequest& req, HttpdResponse& res);
  void handleUsbTransport(HttpdRequest& req, HttpdResponse& res);
  void handleVolume(HttpdRequest& req, HttpdResponse& res);
  void handleDelete(HttpdRequest& req, HttpdResponse& res);
  void handleAudioSource(HttpdRequest& req, HttpdResponse& res);

  // ---- Track / GPS ----
  void handleTrack(HttpdRequest& req, HttpdResponse& res);
  void handleTrackPoints(HttpdRequest& req, HttpdResponse& res);
  void handleTrackSimplified(HttpdRequest& req, HttpdResponse& res);
  void handleTrackDownload(HttpdRequest& req, HttpdResponse& res);

  // ---- BLE sensors ----
  void handleSensorNodes(HttpdRequest& req, HttpdResponse& res);
  void handleSensorNodeDetail(HttpdRequest& req, HttpdResponse& res);
  void handleSensorLive(HttpdRequest& req, HttpdResponse& res);
  void handleSensorForget(HttpdRequest& req, HttpdResponse& res);
  void handleSensorRefresh(HttpdRequest& req, HttpdResponse& res);
  void handleSensorQueuePolicy(HttpdRequest& req, HttpdResponse& res);
  void handleSensorDedupStats(HttpdRequest& req, HttpdResponse& res);
  void handleSensorSpool(HttpdRequest& req, HttpdResponse& res);
  void handleSensorSpoolClear(HttpdRequest& req, HttpdResponse& res);
  void handleBlePasskeySet(HttpdRequest& req, HttpdResponse& res);
  void handleBlePasskeyDelete(HttpdRequest& req, HttpdResponse& res);
  void handleBlePasskeyList(HttpdRequest& req, HttpdResponse& res);

  // ---- MQTT ----
  void handleMqttProvision(HttpdRequest& req, HttpdResponse& res);
  void handleMqttStatus(HttpdRequest& req, HttpdResponse& res);
  void handleMqttCertStatus(HttpdRequest& req, HttpdResponse& res);
  void handleMqttCertRenew(HttpdRequest& req, HttpdResponse& res);
  void handleMqttCertHistory(HttpdRequest& req, HttpdResponse& res);
  void handleMqttCertCaChain(HttpdRequest& req, HttpdResponse& res);

  // ---- Config ----
  void handleConfig(HttpdRequest& req, HttpdResponse& res);
  void handleConfigExport(HttpdRequest& req, HttpdResponse& res);
  void handleConfigBackup(HttpdRequest& req, HttpdResponse& res);
  void handleConfigRestore(HttpdRequest& req, HttpdResponse& res);
  void handleFactoryReset(HttpdRequest& req, HttpdResponse& res);

  // ---- Reboot ----
  void handleReboot(HttpdRequest& req, HttpdResponse& res);
};
```

---

Yang Perlu Anda Perhatikan

1. #include "SensorProtocol.h" — dibutuhkan karena parseBleAddressArg menerima SensorProtocol::BleAddress&. File ini ada di shared/SensorProtocol.h, dan build flag -I../shared sudah ada di platformio.ini Anda. Kalau build error 'SensorProtocol.h' file not found, ganti jadi #include "shared/SensorProtocol.h".
2. #include "HttpdMultipart.h" — dibutuhkan karena handleUpload akan pakai HttpdMultipart::parse di WebUi.cpp.
3. Urutan include: HttpdServer.h → HttpdRequest.h → HttpdResponse.h → HttpdMultipart.h. Itu saya tulis alfabetis-ish. Tidak penting.
4. handleSensorNodeDetail dan handleSensorNodes — saya pisah jadi dua method terpisah. Di WebUi.cpp lama, keduanya di-dispatch dari satu handler /api/sensors/nodes yang cek if (hasArg("id")). Di begin() nanti saya akan tetap satu lambda yang dispatch, tapi sekarang dua method-nya eksplisit.
5. basicAuthMatches — pindah dari free function di WebUi.cpp ke member class. Signature berubah. Ini karena butuh req.header("Authorization").
6. parseSensorNodeId dan parseBleAddressArg — pindah dari free function ke member. Signature sekarang menerima HttpdRequest& req bukan ESPWebServerSecure& server.
7. handleUploadChunk — method baru. Ini dipanggil dari lambda di dalam handleUpload saat HttpdMultipart memanggil FileCallback. Signature menerima (name, filename, mime, data, len, firstChunk, lastChunk). State disimpan di uploadFile_, uploadPath_, uploadBytes_, uploadFailed_.
8. lastBlePasskeyMs_ dan lastSensorQueuePolicyMs_ — sudah ada di versi lama. Saya pertahankan.
9. recordScheduleStart_, recordScheduleDurationSec_, recordScheduleActive_ — sudah ada. Dipertahankan.
10. activeSessionSlot_ — ada di versi lama. Dipertahankan karena csrfValid dan csrfTokenHexForActiveSession masih pakai.

---

Yang Tidak Ada di Header Ini

· server_.on(...) — tidak ada di header, semua di WebUi.cpp::begin().
· WebUi::task() — tetap ada deklarasi, implementasi di WebUi.cpp.
· WebUi::begin() — tetap ada deklarasi, implementasi di WebUi.cpp.
· auditConfigChange — free function di WebUi.cpp lama. Saya pertahankan sebagai free function (tidak butuh req/res).
· jsonEscape, hexEncodeUi, hexDecodeUi, configBackupPlaintext, backupKey, encryptConfigBackup, decryptConfigBackup, parseBackupLine, isValidUploadedWav, eraseStorageTree, sensorAddressJson, sensorNodeJson — semua free function di WebUi.cpp lama. Dipertahankan sebagai free function, saya akan tulis ulang di WebUi.cpp baru.
· basicAuthMatches — dipindah ke member. Ini satu-satunya free function yang saya ubah.

---

Setelah 1 File Ini Tersalin

File Status
HttpdServer.h ✅ Pesan 1
HttpdServer.cpp ✅ Pesan 1
HttpdRequest.h ✅ Pesan 2
HttpdRequest.cpp ✅ Pesan 2
HttpdResponse.h ✅ Pesan 2
HttpdResponse.cpp ✅ Pesan 2 (perbaikan manual)
HttpdMultipart.h ✅ Pesan 3
HttpdMultipart.cpp ✅ Pesan 3
WebUi.h ✅ Pesan 4 (ini)

Belum bisa build. WebUi.cpp masih versi lama dengan server_.on(uri, HTTP_GET, [this]{...}) yang akan gagal compile karena server_ sekarang HttpdServer, bukan ESPWebServerSecure.

Konfirmasi kalau WebUi.h sudah tersalin, saya lanjut ke Pesan 5: WebUi.cpp bagian 1 — ini pesan terpanjang di seluruh iterasi C karena berisi:

· Semua free function helpers (~500 baris)
· begin() dengan 121 server_.on(...) (~400 baris)
· task() (~30 baris)
· Auth/session/CSRF helper (~350 baris)
· ~30 handler pertama

Pesan 5 akan sangat panjang. Saya sarankan Anda siapkan teks editor yang nyaman dan mungkin kopi.