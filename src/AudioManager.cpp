#include "AudioManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include <Wire.h>
#include <math.h>
#include <SD.h>
#include <driver/i2s.h>
#include "Wm8962Codec.h"
#include <esp_audio_simple_dec.h>
#include <esp_audio_types.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "StorageManager.h"
extern StorageManager storage;

static const i2s_port_t AUDIO_I2S_PORT = I2S_NUM_0;
static Wm8962Codec codec;
AudioManager* AudioManager::instance_ = nullptr;

static bool configureCodecForI2S() {
  // WM8962 is the I2S master. The codec driver owns its clock tree and
  // analogue power/mixer setup; ESP32-S3 remains an I2S slave.
  return codec.configureI2sMaster(Config::AUDIO_SAMPLE_RATE, 16);
}

bool AudioManager::initCodec() {
  Wire.begin(Board::I2C_SDA, Board::I2C_SCL, 400000);
  Wire.setTimeOut(50);
  if (!codec.begin(Wire, Board::WM8962_I2C_ADDR)) return false;
  if (!codec.setClassDConfig(gConfig.classDEnabled, gConfig.classDBoostLevel)) return false;
  return configureCodecForI2S();
}

bool AudioManager::initI2S() {
  i2s_config_t cfg{};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_SLAVE | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = Config::AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_I2S_MSB;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = Config::I2S_DMA_BUF_COUNT;
  cfg.dma_buf_len = Config::I2S_DMA_BUF_LEN;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  if (i2s_driver_install(AUDIO_I2S_PORT, &cfg, 0, nullptr) != ESP_OK)
    return false;

  i2s_pin_config_t pins{};
  pins.bck_io_num = Board::I2S_BCLK;
  pins.ws_io_num = Board::I2S_LRCLK;
  pins.data_out_num = Board::I2S_DOUT;
  pins.data_in_num = Board::I2S_DIN;
  if (i2s_set_pin(AUDIO_I2S_PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(AUDIO_I2S_PORT);
    return false;
  }

  i2s_zero_dma_buffer(AUDIO_I2S_PORT);
  return true;
}


bool AudioManager::initAec() {
  if (!aecMic_ || !aecRef_ || !aecOut_) {
    const bool usePsram = psramFound();
    if (usePsram) {
      aecMic_ = static_cast<int16_t*>(
          heap_caps_malloc(sizeof(aecMicFallback_), MALLOC_CAP_SPIRAM));
      aecRef_ = static_cast<int16_t*>(
          heap_caps_malloc(sizeof(aecRefFallback_), MALLOC_CAP_SPIRAM));
      aecOut_ = static_cast<int16_t*>(
          heap_caps_malloc(sizeof(aecOutFallback_), MALLOC_CAP_SPIRAM));
      if (!aecMic_ || !aecRef_ || !aecOut_) {
        if (aecMic_) heap_caps_free(aecMic_);
        if (aecRef_) heap_caps_free(aecRef_);
        if (aecOut_) heap_caps_free(aecOut_);
        aecMic_ = aecRef_ = aecOut_ = nullptr;
      }
    }
    if (!aecMic_) aecMic_ = aecMicFallback_;
    if (!aecRef_) aecRef_ = aecRefFallback_;
    if (!aecOut_) aecOut_ = aecOutFallback_;
  }

  if (!Config::AEC_ENABLED_BY_DEFAULT) {
    aecEnabled_ = false;
    return true;
  }

  aec_ = aec_create(Config::AEC_SAMPLE_RATE, Config::AEC_FILTER_LENGTH, 1,
                    AEC_MODE_FD_LOW_COST);
  if (!aec_) return false;

  const int frameSize = aec_get_chunksize(aec_);
  if (frameSize <= 0 || frameSize > static_cast<int>(Config::AEC_FRAME_SAMPLES)) {
    aec_destroy(aec_);
    aec_ = nullptr;
    return false;
  }
  aecFrameSize_ = static_cast<uint16_t>(frameSize);
  aecEnabled_ = true;
  return true;
}

void AudioManager::deinitAec() {
  if (aec_) {
    aec_destroy(aec_);
    aec_ = nullptr;
  }
  aecFrameSize_ = 0;

  if (aecMic_ && aecMic_ != aecMicFallback_) heap_caps_free(aecMic_);
  if (aecRef_ && aecRef_ != aecRefFallback_) heap_caps_free(aecRef_);
  if (aecOut_ && aecOut_ != aecOutFallback_) heap_caps_free(aecOut_);
  aecMic_ = nullptr;
  aecRef_ = nullptr;
  aecOut_ = nullptr;
}

void AudioManager::updateUsbSampleRate(size_t len) {
  // usb_device_uac 1.3.1 exposes a fixed descriptor and explicitly does not
  // implement host-driven dynamic sample-rate negotiation. Detect the packet
  // size nevertheless so the firmware can diagnose non-native host rates.
  const size_t frames = len / (Config::AUDIO_CHANNELS * sizeof(int16_t));
  if (frames >= 90 && frames <= 220) {
    if (frames >= 188) usbSampleRate_ = 48000;
    else if (frames >= 172) usbSampleRate_ = 44100;
    else if (frames >= 120) usbSampleRate_ = 32000;
    else usbSampleRate_ = 24000;
  }
  StateLock lock(gState);
  if (lock.ok()) {
    gState.usbSampleRate = usbSampleRate_;
    gState.aecEnabled = aecEnabled_;
  }
}

void AudioManager::queueUsbAecReference(const uint8_t* data, size_t len,
                                          uint32_t inputRate) {
  if (!aecEnabled_ || !aec_ || !aecRefBuffer_ || !data ||
      len < Config::AUDIO_CHANNELS * sizeof(int16_t) ||
      inputRate == 0) return;

  const size_t frames = len / (Config::AUDIO_CHANNELS * sizeof(int16_t));
  const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
  uint32_t phase = usbRatePhase_;

  // Convert the exact far-end playback samples to mono 16 kHz. The playback
  // buffer is never modified; only the AEC reference copy is resampled.
  for (size_t i = 0; i < frames; ++i) {
    phase += Config::AEC_SAMPLE_RATE;
    if (phase < inputRate) continue;
    phase -= inputRate;

    int32_t mono = pcm[i * Config::AUDIO_CHANNELS];
    if (Config::AUDIO_CHANNELS > 1)
      mono = (mono + pcm[i * Config::AUDIO_CHANNELS + 1]) / 2;
    const int16_t sample = static_cast<int16_t>(constrain(mono, -32768, 32767));
    if (xStreamBufferSend(aecRefBuffer_, &sample, sizeof(sample), 0) != sizeof(sample)) {
      StateLock lock(gState);
      if (lock.ok()) gState.audioDrops++;
    }
  }
  usbRatePhase_ = phase;
}


void AudioManager::logEvent(const char* event, const String& detail) {
  static uint32_t lastWriteMs = 0;
  static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
  const uint32_t now = millis();
  portENTER_CRITICAL(&logMux);
  const bool throttled = (now - lastWriteMs) < 100U;
  if (!throttled) lastWriteMs = now;
  portEXIT_CRITICAL(&logMux);
  if (throttled || !event || !*event || !storage.ready()) return;

  SpiLock spiLock(pdMS_TO_TICKS(50));
  if (!spiLock.ok()) return;
  if (!SD.exists("/LOG")) (void)SD.mkdir("/LOG");
  const char* path = "/LOG/AUDIO.LOG";
  File f = SD.open(path, FILE_APPEND);
  if (f && f.size() >= Config::AUDIO_LOG_ROTATE_BYTES) {
    f.close();
    if (SD.exists("/LOG/AUDIO.1.LOG")) SD.remove("/LOG/AUDIO.1.LOG");
    if (SD.exists(path)) SD.rename(path, "/LOG/AUDIO.1.LOG");
    f = SD.open(path, FILE_APPEND);
  }
  if (!f) return;
  const time_t epoch = time(nullptr);
  f.printf("%lld,%s,%s\n", static_cast<long long>(epoch), event, detail.c_str());
  f.close();
}

bool AudioManager::begin() {
  instance_ = this;
  recordQuality_ = gConfig.audioRecordQuality;
  mutex_ = xSemaphoreCreateMutex();
  i2sMutex_ = xSemaphoreCreateMutex();
  usbRecordBuffer_ = xStreamBufferCreate(Config::USB_RECORD_BUFFER_BYTES, 1);
  usbMicBuffer_ = xStreamBufferCreate(Config::USB_MIC_BUFFER_BYTES, 1);
  playbackBuffer_ = xStreamBufferCreate(Config::PLAYBACK_PREBUFFER_BYTES, 1);
  usbTransportBuffer_ = xStreamBufferCreate(Config::USB_TRANSPORT_BUFFER_BYTES, 1);
  aecRefStorage_ = psramFound()
      ? static_cast<uint8_t*>(heap_caps_malloc(Config::USB_AEC_REFERENCE_BYTES,
                                               MALLOC_CAP_SPIRAM))
      : aecRefStorageFallback_;
  if (!aecRefStorage_) aecRefStorage_ = aecRefStorageFallback_;
  aecRefBuffer_ = xStreamBufferCreateStatic(
      Config::USB_AEC_REFERENCE_BYTES, sizeof(int16_t), aecRefStorage_,
      &aecRefBufferStatic_);
  if (!mutex_ || !i2sMutex_ || !usbRecordBuffer_ || !usbMicBuffer_ || !playbackBuffer_ || !usbTransportBuffer_ || !aecRefBuffer_) {
    if (usbRecordBuffer_) { vStreamBufferDelete(usbRecordBuffer_); usbRecordBuffer_ = nullptr; }
    if (usbMicBuffer_) { vStreamBufferDelete(usbMicBuffer_); usbMicBuffer_ = nullptr; }
    if (playbackBuffer_) { vStreamBufferDelete(playbackBuffer_); playbackBuffer_ = nullptr; }
    if (usbTransportBuffer_) { vStreamBufferDelete(usbTransportBuffer_); usbTransportBuffer_ = nullptr; }
    aecRefBuffer_ = nullptr;
    if (aecRefStorage_ && aecRefStorage_ != aecRefStorageFallback_) heap_caps_free(aecRefStorage_);
    aecRefStorage_ = nullptr;
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
    if (i2sMutex_) { vSemaphoreDelete(i2sMutex_); i2sMutex_ = nullptr; }
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "Audio mutex init failed";
    return false;
  }
  if (!initCodec()) {
    if (usbRecordBuffer_) { vStreamBufferDelete(usbRecordBuffer_); usbRecordBuffer_ = nullptr; }
    if (usbMicBuffer_) { vStreamBufferDelete(usbMicBuffer_); usbMicBuffer_ = nullptr; }
    if (playbackBuffer_) { vStreamBufferDelete(playbackBuffer_); playbackBuffer_ = nullptr; }
    if (usbTransportBuffer_) { vStreamBufferDelete(usbTransportBuffer_); usbTransportBuffer_ = nullptr; }
    aecRefBuffer_ = nullptr;
    if (aecRefStorage_ && aecRefStorage_ != aecRefStorageFallback_) heap_caps_free(aecRefStorage_);
    aecRefStorage_ = nullptr;
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
    if (i2sMutex_) { vSemaphoreDelete(i2sMutex_); i2sMutex_ = nullptr; }
    StateLock lock(gState);
    if (lock.ok()) {
      gState.codecReady = false;
      gState.lastError = "WM8962 init failed";
    }
    return false;
  }
  if (!initI2S()) {
    if (usbRecordBuffer_) { vStreamBufferDelete(usbRecordBuffer_); usbRecordBuffer_ = nullptr; }
    if (usbMicBuffer_) { vStreamBufferDelete(usbMicBuffer_); usbMicBuffer_ = nullptr; }
    if (playbackBuffer_) { vStreamBufferDelete(playbackBuffer_); playbackBuffer_ = nullptr; }
    if (usbTransportBuffer_) { vStreamBufferDelete(usbTransportBuffer_); usbTransportBuffer_ = nullptr; }
    aecRefBuffer_ = nullptr;
    if (aecRefStorage_ && aecRefStorage_ != aecRefStorageFallback_) heap_caps_free(aecRefStorage_);
    aecRefStorage_ = nullptr;
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
    if (i2sMutex_) { vSemaphoreDelete(i2sMutex_); i2sMutex_ = nullptr; }
    StateLock lock(gState);
    if (lock.ok()) {
      gState.codecReady = false;
      gState.lastError = "I2S init failed";
    }
    return false;
  }

  if (!initAec()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "ESP-SR AEC init failed; continuing without AEC";
    aecEnabled_ = false;
  }

  initialized_ = true;
  (void)setRecordSource(gConfig.audioRecordSource);
  (void)setVox(gConfig.voxEnabled, gConfig.voxThreshold, gConfig.voxHangMs);
  (void)setAec(gConfig.aecEnabled);
  (void)setUsbMonitor(gConfig.usbMonitor);
  (void)setUsbPlaybackTransport(gConfig.usbPlaybackTransport);
  (void)setLoopback(gConfig.audioLoopback);
  setVolume(gConfig.volume);
  StateLock lock(gState);
  if (lock.ok()) {
    gState.codecReady = true;
    gState.aecEnabled = aecEnabled_;
    gState.usbSampleRate = usbSampleRate_;
  }
  return true;
}

bool AudioManager::writeWavHeader(File& f, uint32_t dataBytes) {
  if (!f) return false;

  const uint32_t sampleRate =
      recordQuality_ == 0 ? 8000U : (recordQuality_ == 1 ? 16000U : 44100U);
  const uint16_t channels = recordQuality_ == 2 ? 2U : 1U;
  const uint16_t blockAlign = static_cast<uint16_t>(channels * 2U);
  const uint32_t byteRate = sampleRate * blockAlign;
  uint8_t h[44] = {
    'R','I','F','F',0,0,0,0,'W','A','V','E',
    'f','m','t',' ',16,0,0,0,1,0,0,0,
    0,0,0,0,0,0,0,0,0,0,16,0,
    'd','a','t','a',0,0,0,0
  };
  uint32_t riff = 36 + dataBytes;
  memcpy(h + 4, &riff, 4);
  memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &sampleRate, 4);
  memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &blockAlign, 2);
  memcpy(h + 40, &dataBytes, 4);
  if (!f.seek(0)) return false;
  return f.write(h, sizeof(h)) == sizeof(h);
}

bool AudioManager::startRecording() {
  if (!initialized_ || !mutex_ ||
      xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;

  bool allowed = false;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      allowed = gState.storageReady && !gState.recording &&
                !playing_ && !recordFile_ &&
                (recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC ||
                 gState.usbAudioReady);
    }
  }

  bool ok = false;
  String path;
  String error;
  if (allowed) {
    // Reserve enough headroom for the complete maximum-length PCM WAV, not
    // merely the minimum free-space margin. This prevents a recording from
    // starting successfully and then failing part-way through when the SD
    // card cannot hold the configured maximum segment.
    constexpr uint64_t maxPcmBytes =
        static_cast<uint64_t>(Config::AUDIO_SAMPLE_RATE) *
        Config::AUDIO_CHANNELS * sizeof(int16_t) *
        min<uint32_t>(Config::RECORD_SPLIT_SECONDS, Config::RECORD_MAX_SECONDS);
    const uint32_t requiredBytes = static_cast<uint32_t>(
        min<uint64_t>(UINT32_MAX - 44U, maxPcmBytes) + 44U);
    if (!storage.prepareRecordingSpace(requiredBytes)) {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "Insufficient SD recording space";
      xSemaphoreGive(mutex_);
      return false;
    }
    {
      SpiLock spiLock(pdMS_TO_TICKS(100));
      if (!spiLock.ok()) {
        error = "SPI mutex unavailable";
      } else if (!SD.exists("/REC") && !SD.mkdir("/REC")) {
        error = "REC directory create failed";
      } else {
        const uint32_t stamp = millis();
        for (uint16_t attempt = 0; attempt < 100; ++attempt) {
          path = "/REC/REC_" + String(stamp);
          if (attempt != 0) path += "_" + String(attempt);
          path += ".WAV";
          if (!SD.exists(path)) break;
          path = "";
        }
        if (path.isEmpty()) {
          error = "WAV filename allocation failed";
        } else {
          recordFile_ = SD.open(path, FILE_WRITE);
          if (recordFile_) recordPath_ = path;
        }
        if (!recordFile_) {
          error = "WAV create failed";
        } else {
          uint8_t zero[44] = {};
          if (recordFile_.write(zero, sizeof(zero)) != sizeof(zero)) {
            recordFile_.close();
            (void)SD.remove(path);
            recordPath_ = "";
            error = "WAV header reserve failed";
          } else {
            recordedBytes_ = 0;
            recordStartedMs_ = millis();
            recordingPaused_ = false;
            recordingPart_ = 0;
            ok = true;
          }
        }
      }
    }

    // SPI must be released before taking the global state mutex.
    if (ok) {
      bool stillAllowed = false;
      {
        StateLock lock(gState);
        if (lock.ok()) {
          stillAllowed = gState.storageReady && !gState.recording &&
                         !playing_ &&
                         (recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC ||
                          gState.usbAudioReady);
        } else {
          error = "State mutex unavailable";
        }
      }

      if (!stillAllowed) {
        discardRecordingFile();
        ok = false;
        if (error.isEmpty()) error = "Recording state changed";
      }
    }
  }

  if (ok) {
    bool committed = false;
    {
      StateLock lock(gState);
      if (lock.ok()) {
        if (gState.storageReady && !gState.recording &&
            !playing_ &&
            (recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC ||
             gState.usbAudioReady)) {
          gState.recording = true;
          gState.lastAudioFile = path;
          committed = true;
        } else {
          error = "Recording state changed";
        }
      } else {
        error = "State mutex unavailable";
      }
    }

    if (!committed) {
      discardRecordingFile();
      ok = false;
    } else {
      captureUsbRecord_ = recordSource_ == Config::AUDIO_SOURCE_USB;
      captureWm8962Mic_ = recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC;
      if (usbRecordBuffer_) (void)xStreamBufferReset(usbRecordBuffer_);
      if (usbMicBuffer_) (void)xStreamBufferReset(usbMicBuffer_);
      if (aecRefBuffer_) (void)xStreamBufferReset(aecRefBuffer_);
      logEvent("REC_START", path);
    }
  }

  if (!ok && !error.isEmpty()) {
    logEvent("ERROR", error);
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = error;
  }

  xSemaphoreGive(mutex_);
  return ok;
}


bool AudioManager::routeInput(uint8_t source) {
  if (source > Config::AUDIO_SOURCE_USB) return false;
  // WM8962 uses its native input mixer. USB recording does not change the
  // analogue path, so leave the codec on the last selected local input.
  if (source == Config::AUDIO_SOURCE_USB) return true;
  return codec.routeInput(source);
}

void AudioManager::updateAudioLevel(const uint8_t* data, size_t len) {
  if (!data || len < 2) return;
  const size_t samples = len / sizeof(int16_t);
  double sum = 0.0;
  int32_t peak = 0;
  const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
  for (size_t i = 0; i < samples; ++i) {
    const int32_t v = pcm[i];
    const int32_t a = v < 0 ? -v : v;
    if (a > peak) peak = a;
    sum += static_cast<double>(v) * static_cast<double>(v);
  }
  const float peakNorm = static_cast<float>(peak) / 32768.0f;
  const float rmsNorm = static_cast<float>(sqrt(sum / static_cast<double>(samples))) / 32768.0f;
  StateLock lock(gState);
  if (lock.ok()) {
    gState.audioPeak = peakNorm;
    gState.audioRms = rmsNorm;
    gState.audioClipped = peak >= 32700;
  }
}

bool AudioManager::setRecordSource(uint8_t source) {
  if (source > Config::AUDIO_SOURCE_USB) return false;
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  bool busy = false;
  {
    StateLock lock(gState);
    if (lock.ok()) busy = gState.recording || gState.playing;
  }
  if (busy) {
    if (mutex_) xSemaphoreGive(mutex_);
    return false;
  }

  if (!routeInput(source)) {
    if (mutex_) xSemaphoreGive(mutex_);
    return false;
  }
  recordSource_ = source;
  captureUsbRecord_ = false;
  captureWm8962Mic_ = recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC;
  if (usbRecordBuffer_) (void)xStreamBufferReset(usbRecordBuffer_);
  if (usbMicBuffer_) (void)xStreamBufferReset(usbMicBuffer_);

  if (mutex_) xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::setUsbMonitor(bool enabled) {
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  usbMonitor_ = enabled;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.usbMonitor = enabled;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return true;
}


bool AudioManager::setUsbPlaybackTransport(bool enabled) {
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  usbPlaybackTransport_ = enabled;
  if (usbTransportBuffer_) (void)xStreamBufferReset(usbTransportBuffer_);
  {
    StateLock lock(gState);
    if (lock.ok()) gState.usbPlaybackTransport = enabled;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::setLoopback(bool enabled) {
  if (!initialized_) return false;
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool busy = false;
  {
    StateLock lock(gState);
    if (lock.ok()) busy = gState.recording || gState.playing || gState.usbAudioActive;
  }
  if (busy) {
    if (mutex_) xSemaphoreGive(mutex_);
    return false;
  }
  const bool codecOk = codec.setLoopback(enabled);
  if (!codecOk) {
    if (mutex_) xSemaphoreGive(mutex_);
    return false;
  }
  loopback_ = enabled;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.audioLoopback = enabled;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return true;
}


static uint8_t pcm16ToMulaw(int16_t pcm) {
  constexpr int BIAS = 0x84;
  constexpr int CLIP = 32635;
  int32_t sample = pcm;
  int sign = (sample < 0) ? 0x80 : 0;
  if (sample < 0) sample = -sample;
  if (sample > CLIP) sample = CLIP;
  sample += BIAS;
  int exponent = 7;
  for (int mask = 0x4000; exponent > 0 && !(sample & mask); mask >>= 1) --exponent;
  const int mantissa = (sample >> (exponent + 3)) & 0x0F;
  return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
}

static int16_t mulawToPcm16(uint8_t u) {
  u = static_cast<uint8_t>(~u);
  int t = ((u & 0x0F) << 3) + 0x84;
  t <<= ((u & 0x70) >> 4);
  return static_cast<int16_t>((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

bool AudioManager::captureVoiceFrame(uint8_t* out, size_t capacity, size_t& written) {
  written = 0;
  constexpr size_t voiceSamples = 160; // 20 ms @ 8 kHz
  constexpr size_t micFrames = (Config::AUDIO_SAMPLE_RATE * 40U) / 1000U;
  if (!out || capacity < 4 + voiceSamples || !initialized_ || !i2sMutex_ ||
      !aecMic_ || !aecRef_ || !aecOut_) return false;
  if (xSemaphoreTake(i2sMutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;

  uint8_t pcm[micFrames * Config::AUDIO_CHANNELS * sizeof(int16_t)];
  size_t got = 0;
  const esp_err_t err = i2s_read(AUDIO_I2S_PORT, pcm, sizeof(pcm), &got,
                                  pdMS_TO_TICKS(30));
  xSemaphoreGive(i2sMutex_);
  if (err != ESP_OK || got != sizeof(pcm)) return false;

  const int16_t* samples = reinterpret_cast<const int16_t*>(pcm);
  size_t aecFrames = min(
      static_cast<size_t>(aecFrameSize_ ? aecFrameSize_ : Config::AEC_FRAME_SAMPLES),
      static_cast<size_t>(Config::AEC_FRAME_SAMPLES));
  if (aecFrames == 0) {
    aecFrames = Config::AEC_FRAME_SAMPLES;
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "AEC frame size invalid; using default";
  }
  if (aecFrames == 0 || !aecMic_ || !aecRef_ || !aecOut_) return false;
  for (size_t i = 0; i < aecFrames; ++i) {
    const size_t src = min(micFrames - 1,
        static_cast<size_t>((static_cast<uint64_t>(i) * Config::AUDIO_SAMPLE_RATE) /
                            Config::AEC_SAMPLE_RATE));
    const int32_t mono =
        (static_cast<int32_t>(samples[src * 2]) +
         static_cast<int32_t>(samples[src * 2 + 1])) / 2;
    aecMic_[i] = static_cast<int16_t>(constrain(mono, -32768, 32767));
  }

  const int16_t* clean = aecMic_;
  const size_t aecBytes = aecFrames * sizeof(int16_t);
  if (aecEnabled_ && aec_ && aecRefBuffer_ &&
      xStreamBufferBytesAvailable(aecRefBuffer_) >= aecBytes &&
      xStreamBufferReceive(aecRefBuffer_, aecRef_, aecBytes, 0) == aecBytes) {
    aec_process(aec_, aecMic_, aecRef_, aecOut_);
    clean = aecOut_;
  }

  out[0] = 0x56;
  out[1] = 1; // μ-law 8 kHz mono.
  out[2] = static_cast<uint8_t>(Config::VOICE_FRAME_MS);
  out[3] = 0;
  for (size_t i = 0; i < voiceSamples; ++i) {
    // 16 kHz AEC output -> 8 kHz voice transport.
    out[4 + i] = pcm16ToMulaw(clean[min(aecFrames - 1, i * 2)]);
  }
  written = 4 + voiceSamples;
  return true;
}


bool AudioManager::playVoiceFrame(const uint8_t* data, size_t len) {
  if (!data || len != 168 || data[0] != 0x56 || data[1] != 1 ||
      data[2] != Config::VOICE_FRAME_MS || !initialized_ || !i2sMutex_) return false;
  const uint16_t expectedCrc = static_cast<uint16_t>(data[166]) |
                               (static_cast<uint16_t>(data[167]) << 8);
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < 166; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b)
      crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) :
                        static_cast<uint16_t>(crc >> 1);
  }
  if (crc != expectedCrc) return false;
  constexpr size_t outFrames = (Config::AUDIO_SAMPLE_RATE * Config::VOICE_FRAME_MS) / 1000U;
  int16_t pcm[outFrames * Config::AUDIO_CHANNELS];
  for (size_t i = 0; i < outFrames; ++i) {
    const size_t src = min<size_t>(159, (i * 8000U) / Config::AUDIO_SAMPLE_RATE);
    const int16_t sample = mulawToPcm16(data[6 + src]);
    pcm[i * 2] = sample;
    pcm[i * 2 + 1] = sample;
  }
  size_t writtenBytes = 0;
  if (xSemaphoreTake(i2sMutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  const esp_err_t err = i2s_write(AUDIO_I2S_PORT, pcm, sizeof(pcm),
                                  &writtenBytes, pdMS_TO_TICKS(30));
  xSemaphoreGive(i2sMutex_);
  if (err == ESP_OK && writtenBytes == sizeof(pcm))
    queueUsbAecReference(reinterpret_cast<const uint8_t*>(pcm), sizeof(pcm),
                          Config::AUDIO_SAMPLE_RATE);
  return err == ESP_OK && writtenBytes == sizeof(pcm);
}

bool AudioManager::playTone(uint16_t frequencyHz, uint16_t durationMs, uint8_t percent) {
  if (!initialized_ || frequencyHz == 0 || frequencyHz > 10000 ||
      durationMs == 0 || durationMs > Config::AUDIO_TONE_MAX_MS) return false;
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool busy = false;
  {
    StateLock lock(gState);
    if (lock.ok()) busy = gState.recording || gState.playing || gState.usbAudioActive;
  }
  if (busy) {
    if (mutex_) xSemaphoreGive(mutex_);
    return false;
  }

  const uint32_t frames = (Config::AUDIO_SAMPLE_RATE * durationMs) / 1000U;
  const float amplitude = 32767.0f * (constrain(percent, 0, 100) / 100.0f);
  const float phaseStep = 2.0f * PI * static_cast<float>(frequencyHz) /
                          static_cast<float>(Config::AUDIO_SAMPLE_RATE);
  uint8_t buffer[Config::AUDIO_IO_BYTES];
  uint32_t frame = 0;
  float phase = 0.0f;
  bool ok = true;
  while (frame < frames) {
    const uint32_t framesThis = min<uint32_t>(
        frames - frame, Config::AUDIO_IO_BYTES / (Config::AUDIO_CHANNELS * sizeof(int16_t)));
    int16_t* pcm = reinterpret_cast<int16_t*>(buffer);
    for (uint32_t i = 0; i < framesThis; ++i) {
      const int16_t sample = static_cast<int16_t>(sinf(phase) * amplitude);
      pcm[2 * i] = sample;
      pcm[2 * i + 1] = sample;
      phase += phaseStep;
      if (phase >= 2.0f * PI) phase -= 2.0f * PI;
    }
    const size_t bytes = framesThis * Config::AUDIO_CHANNELS * sizeof(int16_t);
    size_t written = 0;
    if (!i2sMutex_ || xSemaphoreTake(i2sMutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
      ok = false;
      break;
    }
    const esp_err_t err = i2s_write(AUDIO_I2S_PORT, buffer, bytes, &written, pdMS_TO_TICKS(50));
    xSemaphoreGive(i2sMutex_);
    if (err != ESP_OK || written != bytes) {
      ok = false;
      break;
    }
    frame += framesThis;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return ok;
}

bool AudioManager::writeRecordingData(const uint8_t* data, size_t len) {
  if (!data || !len || !recordFile_ || (len & 3U)) return false;

  SpiLock spiLock(pdMS_TO_TICKS(20));
  if (!spiLock.ok()) {
    StateLock lock(gState);
    if (lock.ok()) gState.audioDrops++;
    return false;
  }

  if (recordQuality_ == 2) {
    const size_t written = recordFile_.write(data, len);
    recordedBytes_ += static_cast<uint32_t>(written);
    if (written != len) {
      StateLock lock(gState);
      if (lock.ok()) {
        gState.audioDrops++;
        gState.lastError = "WAV write failed";
      }
      return false;
    }
    return true;
  }

  const uint32_t inRate = recordSource_ == Config::AUDIO_SOURCE_USB
      ? max<uint32_t>(8000U, usbSampleRate_) : Config::AUDIO_SAMPLE_RATE;
  const uint32_t outRate = recordQuality_ == 0 ? 8000U : 16000U;
  uint8_t mono[2];
  size_t produced = 0;
  for (size_t i = 0; i < len; i += 4) {
    // I2S/USB PCM is signed 16-bit stereo, little-endian.
    const int16_t l = static_cast<int16_t>(
        static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8));
    const int16_t r = static_cast<int16_t>(
        static_cast<uint16_t>(data[i + 2]) | (static_cast<uint16_t>(data[i + 3]) << 8));
    recordResamplePhase_ += outRate;
    if (recordResamplePhase_ >= inRate) {
      recordResamplePhase_ -= inRate;
      const int32_t m = (static_cast<int32_t>(l) + static_cast<int32_t>(r)) / 2;
      mono[0] = static_cast<uint8_t>(m & 0xff);
      mono[1] = static_cast<uint8_t>((m >> 8) & 0xff);
      if (recordFile_.write(mono, sizeof(mono)) != sizeof(mono)) {
        StateLock lock(gState);
        if (lock.ok()) {
          gState.audioDrops++;
          gState.lastError = "WAV resampled write failed";
        }
        return false;
      }
      produced += sizeof(mono);
    }
  }
  recordedBytes_ += static_cast<uint32_t>(produced);
  return true;
}

bool AudioManager::flushUsbRecordingBuffer() {
  if (!usbRecordBuffer_ || !recordFile_) return true;

  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "SPI mutex unavailable while flushing USB recording";
    return false;
  }

  uint8_t buffer[Config::AUDIO_IO_BYTES];
  while (xStreamBufferBytesAvailable(usbRecordBuffer_) > 0) {
    const size_t got = xStreamBufferReceive(
        usbRecordBuffer_, buffer, sizeof(buffer), 0);
    if (!got) break;
    const size_t written = recordFile_.write(buffer, got);
    recordedBytes_ += static_cast<uint32_t>(written);
    if (written != got) {
      StateLock lock(gState);
      if (lock.ok()) {
        gState.audioDrops++;
        gState.lastError = "USB WAV flush failed";
      }
      return false;
    }
  }
  return true;
}

void AudioManager::discardRecordingFile() {
  if (!recordPath_.isEmpty()) {
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (!spiLock.ok()) return;
    if (recordFile_) recordFile_.close();
    if (SD.remove(recordPath_)) {
      recordPath_ = "";
    }
    return;
  }
  if (recordFile_) {
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (spiLock.ok()) recordFile_.close();
  }
}

bool AudioManager::finalizeWav() {
  if (!recordFile_) return true;

  bool ok = false;
  bool spiOk = false;
  {
    SpiLock spiLock(pdMS_TO_TICKS(100));
    spiOk = spiLock.ok();
    if (spiOk) {
      ok = writeWavHeader(recordFile_, recordedBytes_);
      if (ok) {
        if (!recordFile_.flush()) {
          ok = false;
        } else {
          recordFile_.close();
          recordPath_ = "";
        }
      }
    }
  }

  if (!ok) {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.lastError = spiOk
          ? "WAV header finalize failed"
          : "SPI mutex unavailable while finalizing WAV";
    }
  }
  return ok;
}

bool AudioManager::stopRecording() {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;

  bool recording = false;
  {
    StateLock lock(gState);
    if (lock.ok()) recording = gState.recording;
  }
  bool ok = true;
  if (recording) {
    captureUsbRecord_ = false;
    // Keep WM8962 microphone capture available to the USB input endpoint
    // after recording stops. The capture flag is tied to the selected source,
    // not to the SD recording state.
    captureWm8962Mic_ = recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC;
    if (i2sMutex_ &&
        xSemaphoreTake(i2sMutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
      // Do not leave an open WAV behind when the short I2S critical section
      // cannot be acquired. Stop producers first, mark the recording closed,
      // then finalize the file using the SPI path.
      captureUsbRecord_ = false;
      captureWm8962Mic_ = false;
      {
        StateLock stateLock(gState);
        if (stateLock.ok()) gState.recording = false;
      }
      const bool finalized = finalizeWav();
      xSemaphoreGive(mutex_);
      logEvent(finalized ? "REC_STOP" : "ERROR",
               finalized ? String() : "REC_STOP_FORCED_FINALIZE_FAILED");
      return finalized;
    }
    if (i2sMutex_) xSemaphoreGive(i2sMutex_);
    {
      StateLock lock(gState);
      if (!lock.ok()) {
        xSemaphoreGive(mutex_);
        return false;
      }
      gState.recording = false;
    }
    if (recordSource_ == Config::AUDIO_SOURCE_USB) {
      ok = flushUsbRecordingBuffer();
    }
    if (ok) ok = finalizeWav();
  }

  xSemaphoreGive(mutex_);
  logEvent(ok ? "REC_STOP" : "ERROR", ok ? String() : "REC_STOP_FAILED");
  return ok;
}


bool AudioManager::pauseRecording(bool paused) {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool active = false;
  {
    StateLock lock(gState);
    if (lock.ok()) active = gState.recording;
  }
  if (!active) {
    xSemaphoreGive(mutex_);
    return false;
  }
  recordingPaused_ = paused;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.recordingPaused = paused;
  }
  xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::openRecordingPart() {
  if (!gState.storageReady) return false;
  SpiLock spiLock(pdMS_TO_TICKS(100));
  if (!spiLock.ok()) return false;
  if (!SD.exists("/REC") && !SD.mkdir("/REC")) return false;

  const uint32_t stamp = millis();
  String path;
  for (uint16_t attempt = 0; attempt < 100; ++attempt) {
    path = "/REC/REC_" + String(stamp);
    if (recordingPart_) path += "_P" + String(recordingPart_);
    if (attempt) path += "_" + String(attempt);
    path += ".WAV";
    if (!SD.exists(path)) break;
    path = "";
  }
  if (path.isEmpty()) return false;

  recordFile_ = SD.open(path, FILE_WRITE);
  if (!recordFile_) return false;
  uint8_t zero[44] = {};
  if (recordFile_.write(zero, sizeof(zero)) != sizeof(zero)) {
    recordFile_.close();
    (void)SD.remove(path);
    return false;
  }
  recordPath_ = path;
  recordedBytes_ = 0;
  recordResamplePhase_ = 0;
  recordStartedMs_ = millis();
  if (usbRecordBuffer_) (void)xStreamBufferReset(usbRecordBuffer_);
  if (usbMicBuffer_) (void)xStreamBufferReset(usbMicBuffer_);
  return true;
}

bool AudioManager::splitRecording() {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool active = false;
  {
    StateLock lock(gState);
    if (lock.ok()) active = gState.recording;
  }
  if (!active) {
    xSemaphoreGive(mutex_);
    return false;
  }

  captureUsbRecord_ = false;
  captureWm8962Mic_ = false;
  if (recordSource_ == Config::AUDIO_SOURCE_USB && !flushUsbRecordingBuffer()) {
    xSemaphoreGive(mutex_);
    return false;
  }
  if (!finalizeWav()) {
    xSemaphoreGive(mutex_);
    return false;
  }
  ++recordingPart_;
  if (!openRecordingPart()) {
    xSemaphoreGive(mutex_);
    return false;
  }
  captureUsbRecord_ = recordSource_ == Config::AUDIO_SOURCE_USB;
  captureWm8962Mic_ = recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC;
  {
    StateLock lock(gState);
    if (lock.ok()) {
      gState.recording = true;
      gState.recordingPaused = false;
      gState.lastAudioFile = recordPath_;
    }
  }
  xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::applyRecordQualityRuntime(uint8_t level) {
  if (level > 2 || !initialized_ || !mutex_) return false;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool recording = false; { StateLock lock(gState); if (lock.ok()) recording = gState.recording; }
  if (recording) { xSemaphoreGive(mutex_); return false; }
  recordQuality_ = level;
  xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::setRecordQuality(uint8_t level) {
  RuntimeConfig previous{}; if (!configSnapshot(previous)) return false;
  RuntimeConfig candidate = previous; candidate.audioRecordQuality = level;
  if (!applyRecordQualityRuntime(level)) return false;
  if (configCommit(candidate)) return true;
  (void)applyRecordQualityRuntime(previous.audioRecordQuality);
  return false;
}

bool AudioManager::setAec(bool enabled) {
  if (!initialized_) return false;
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  if (enabled && !aec_) {
    if (!initAec()) {
      if (mutex_) xSemaphoreGive(mutex_);
      return false;
    }
  }
  aecEnabled_ = enabled;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.aecEnabled = enabled;
  }
  if (aecRefBuffer_) (void)xStreamBufferReset(aecRefBuffer_);
  usbRatePhase_ = 0;
  if (mutex_) xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::setVox(bool enabled, float threshold, uint32_t hangMs) {
  if (threshold < 0.005f || threshold > 1.0f ||
      hangMs < 50U || hangMs > 10000U) return false;
  if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  voxEnabled_ = enabled;
  voxThreshold_ = threshold;
  voxHangMs_ = hangMs;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.vox = enabled;
  }
  if (mutex_) xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::setVoxAdapt(uint32_t adaptMs) {
  if (adaptMs != 0 && (adaptMs < 100 || adaptMs > 60000)) return false;
  voxAdaptMs_ = adaptMs;
  voxAdaptStartedMs_ = millis();
  voxNoiseFloor_ = 0.0f;
  return true;
}

bool AudioManager::openPlaybackDecoder(const String& path) {
  closePlaybackDecoder();
  String upper = path;
  upper.toUpperCase();
  esp_audio_simple_dec_type_t type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
  if (upper.endsWith(".MP3")) type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
  else if (upper.endsWith(".OPUS")) type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
  else if (upper.endsWith(".WAV")) type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
  else return false;

  esp_audio_simple_dec_cfg_t cfg{};
  cfg.dec_type = type;
  cfg.dec_cfg = nullptr;
  cfg.cfg_size = 0;
  cfg.use_frame_dec = false;

  esp_audio_simple_dec_handle_t hd = nullptr;
  if (esp_audio_simple_dec_open(&cfg, &hd) != ESP_AUDIO_ERR_OK || !hd) return false;
  simpleDecoder_ = hd;
  decoderType_ = static_cast<uint8_t>(type);
  compressedPlayback_ = type != ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
  playbackEof_ = false;
  playbackPositionMs_ = 0;
  return true;
}

void AudioManager::closePlaybackDecoder() {
  if (simpleDecoder_) {
    esp_audio_simple_dec_close(reinterpret_cast<esp_audio_simple_dec_handle_t>(simpleDecoder_));
    simpleDecoder_ = nullptr;
  }
  decoderType_ = 0;
  compressedPlayback_ = false;
}

bool AudioManager::fillPlaybackBuffer() {
  if (!playFile_ || playbackEof_ || !playbackBuffer_) return false;
  if (xStreamBufferBytesAvailable(playbackBuffer_) >= Config::PLAYBACK_PREBUFFER_BYTES / 2) return true;

  while (xStreamBufferSpacesAvailable(playbackBuffer_) >= sizeof(playbackInput_)) {
    const size_t want = min(sizeof(playbackInput_), static_cast<size_t>(playbackRemaining_));
    if (!want) {
      playbackEof_ = true;
      break;
    }
    size_t n = 0;
    {
      SpiLock spiLock(pdMS_TO_TICKS(20));
      if (!spiLock.ok()) return false;
      n = playFile_.read(playbackInput_, want);
    }
    if (!n) {
      playbackEof_ = true;
      break;
    }
    if (xStreamBufferSend(playbackBuffer_, playbackInput_, n, 0) != n) {
      return false;
    }
    playbackRemaining_ -= static_cast<uint32_t>(n);
    if (n < want) {
      playbackEof_ = true;
      break;
    }
  }
  return xStreamBufferBytesAvailable(playbackBuffer_) != 0 || playbackEof_;
}

bool AudioManager::playDecodedBuffer() {
  if (!simpleDecoder_ || !playbackBuffer_) return false;
  const size_t available = xStreamBufferBytesAvailable(playbackBuffer_);
  if (!available) return playbackEof_;

  const size_t got = xStreamBufferReceive(playbackBuffer_, playbackInput_,
                                          min(available, sizeof(playbackInput_)), 0);
  if (!got) return false;

  esp_audio_simple_dec_raw_t raw{};
  raw.buffer = playbackInput_;
  raw.len = got;
  raw.eos = playbackEof_ && playbackRemaining_ == 0;

  while (raw.len) {
    esp_audio_simple_dec_out_t out{};
    out.buffer = playbackPcm_;
    out.len = sizeof(playbackPcm_);
    const esp_audio_err_t err = esp_audio_simple_dec_process(
        reinterpret_cast<esp_audio_simple_dec_handle_t>(simpleDecoder_), &raw, &out);
    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH || err != ESP_AUDIO_ERR_OK) return false;

    if (out.decoded_size) {
      esp_audio_simple_dec_info_t info{};
      if (esp_audio_simple_dec_get_info(
              reinterpret_cast<esp_audio_simple_dec_handle_t>(simpleDecoder_), &info) == ESP_AUDIO_ERR_OK) {
        playbackSampleRate_ = info.sample_rate;
        playbackChannels_ = info.channel;
      }

      const uint8_t* output = playbackPcm_;
      size_t outputBytes = out.decoded_size;
      uint8_t resampled[16384] = {};
      if (playbackSampleRate_ != Config::AUDIO_SAMPLE_RATE ||
          playbackChannels_ != Config::AUDIO_CHANNELS) {
        const int16_t* in = reinterpret_cast<const int16_t*>(playbackPcm_);
        const size_t inFrames = out.decoded_size /
            (max<uint32_t>(1, playbackChannels_) * sizeof(int16_t));
        const size_t outFrames = static_cast<size_t>(
            (static_cast<uint64_t>(inFrames) * Config::AUDIO_SAMPLE_RATE) /
            max<uint32_t>(1, playbackSampleRate_));
        if (outFrames * Config::AUDIO_CHANNELS * sizeof(int16_t) > sizeof(resampled))
          return false;
        int16_t* dst = reinterpret_cast<int16_t*>(resampled);
        for (size_t i = 0; i < outFrames; ++i) {
          const size_t src = min(inFrames - 1,
              static_cast<size_t>((static_cast<uint64_t>(i) * playbackSampleRate_) /
                                  Config::AUDIO_SAMPLE_RATE));
          const int16_t sampleL = in[src * playbackChannels_];
          const int16_t sampleR = playbackChannels_ > 1 ? in[src * playbackChannels_ + 1] : sampleL;
          dst[i * 2] = sampleL;
          dst[i * 2 + 1] = sampleR;
        }
        output = resampled;
        outputBytes = outFrames * Config::AUDIO_CHANNELS * sizeof(int16_t);
      }

      size_t written = 0;
      if (!i2sMutex_ || xSemaphoreTake(i2sMutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;
      const esp_err_t w = i2s_write(AUDIO_I2S_PORT, output, outputBytes,
                                    &written, pdMS_TO_TICKS(20));
      xSemaphoreGive(i2sMutex_);
      if (w != ESP_OK || written != outputBytes) return false;
      if (usbPlaybackTransport_ && usbTransportBuffer_) {
        const size_t queued = xStreamBufferSend(usbTransportBuffer_, output,
                                                outputBytes, 0);
        if (queued != outputBytes) {
          StateLock lock(gState);
          if (lock.ok()) gState.audioDrops++;
        }
      }

      const uint32_t bytesPerFrame =
          max<uint32_t>(1, playbackChannels_) * sizeof(int16_t);
      if (playbackSampleRate_) {
        playbackPositionMs_ += static_cast<uint32_t>(
            (static_cast<uint64_t>(out.decoded_size) * 1000ULL) /
            (bytesPerFrame * playbackSampleRate_));
      }
    }
    if (raw.consumed == 0) break;
    raw.len -= raw.consumed;
    raw.buffer += raw.consumed;
    raw.consumed = 0;
  }
  return true;
}

bool AudioManager::pausePlayback(bool paused) {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  if (!playing_) {
    xSemaphoreGive(mutex_);
    return false;
  }
  playbackPaused_ = paused;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.playbackPaused = paused;
  }
  xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::seekPlaybackMs(uint32_t positionMs) {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  if (!playing_ || compressedPlayback_ || !playFile_ || playbackSampleRate_ == 0) {
    xSemaphoreGive(mutex_);
    return false;
  }
  const uint64_t bytesPerSec = static_cast<uint64_t>(playbackSampleRate_) *
                               playbackChannels_ * sizeof(int16_t);
  const uint64_t offset = 44ULL + (static_cast<uint64_t>(positionMs) * bytesPerSec / 1000ULL);
  if (offset > playFile_.size() || !playFile_.seek(offset)) {
    xSemaphoreGive(mutex_);
    return false;
  }
  playbackRemaining_ = static_cast<uint32_t>(playFile_.size() - offset);
  playbackPositionMs_ = positionMs;
  if (playbackBuffer_) (void)xStreamBufferReset(playbackBuffer_);
  {
    StateLock lock(gState);
    if (lock.ok()) gState.playbackPositionMs = positionMs;
  }
  xSemaphoreGive(mutex_);
  return true;
}

bool AudioManager::enqueueFile(const String& path) {
  if (path.length() > Config::MAX_PATH || path.indexOf("..") >= 0 ||
      path.indexOf('\\') >= 0 || !path.startsWith("/REC/") ||
      path.lastIndexOf('/') != 4) return false;
  if (!path.endsWith(".WAV") && !path.endsWith(".MP3") && !path.endsWith(".OPUS")) return false;
  if (queueCount_ >= Config::PLAYBACK_QUEUE_DEPTH) return false;
  queue_[queueTail_] = path;
  queueTail_ = static_cast<uint8_t>((queueTail_ + 1) % Config::PLAYBACK_QUEUE_DEPTH);
  ++queueCount_;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.queueDepth = queueCount_;
  }
  return true;
}

void AudioManager::clearQueue() {
  for (auto &item : queue_) item = "";
  queueHead_ = queueTail_ = queueCount_ = 0;
  StateLock lock(gState);
  if (lock.ok()) gState.queueDepth = 0;
}

bool AudioManager::playNextQueued() {
  if (!queueCount_) return false;
  const String next = queue_[queueHead_];
  queue_[queueHead_] = "";
  queueHead_ = static_cast<uint8_t>((queueHead_ + 1) % Config::PLAYBACK_QUEUE_DEPTH);
  --queueCount_;
  {
    StateLock lock(gState);
    if (lock.ok()) gState.queueDepth = queueCount_;
  }
  return playFile(next);
}

bool AudioManager::playFile(const String& path) {
  const bool wav = path.length() >= 4 &&
                   path.substring(path.length() - 4).equalsIgnoreCase(".WAV");
  const bool mp3 = path.length() >= 4 &&
                   path.substring(path.length() - 4).equalsIgnoreCase(".MP3");
  const bool opus = path.length() >= 5 &&
                    path.substring(path.length() - 5).equalsIgnoreCase(".OPUS");
  if (!initialized_ || !mutex_ || path.length() > Config::MAX_PATH ||
      path.indexOf("..") >= 0 || path.indexOf('\\') >= 0 || path.indexOf('\0') >= 0 ||
      !path.startsWith("/REC/") || path.lastIndexOf('/') != 4 ||
      (!wav && !mp3 && !opus) ||
      xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  bool allowed = false;
  {
    StateLock lock(gState);
    if (lock.ok() && gState.storageReady && !gState.usbAudioActive && !gState.recording)
      allowed = true;
  }
  if (!allowed) {
    xSemaphoreGive(mutex_);
    return false;
  }

  playing_ = false;
  playbackPaused_ = false;
  playbackEof_ = false;
  if (playFile_) {
    SpiLock spiLock(pdMS_TO_TICKS(50));
    if (spiLock.ok()) playFile_.close();
  }
  closePlaybackDecoder();
  if (playbackBuffer_) (void)xStreamBufferReset(playbackBuffer_);

  bool opened = false;
  uint32_t offset = 0;
  uint32_t bytes = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t rate = 0;
  {
    SpiLock spiLock(pdMS_TO_TICKS(100));
    if (spiLock.ok()) {
      playFile_ = SD.open(path, FILE_READ);
      opened = static_cast<bool>(playFile_);
      if (opened && wav) {
        /* WAV parser in the codec component accepts PCM and IMA-ADPCM. */
        if (!openPlaybackDecoder(path)) {
          playFile_.close();
          opened = false;
        } else {
          /* The simple decoder owns container parsing; start from byte zero. */
          if (!playFile_.seek(0)) {
            closePlaybackDecoder();
            playFile_.close();
            opened = false;
          } else {
            bytes = static_cast<uint32_t>(playFile_.size());
            offset = 0;
          }
        }
      } else if (opened) {
        if (!openPlaybackDecoder(path) || !playFile_.seek(0)) {
          closePlaybackDecoder();
          playFile_.close();
          opened = false;
        } else {
          bytes = static_cast<uint32_t>(playFile_.size());
          offset = 0;
        }
      }
    }
  }

  if (!opened) {
    xSemaphoreGive(mutex_);
    return false;
  }

  playbackRemaining_ = bytes - offset;
  playbackTotalBytes_ = bytes;
  playbackPositionMs_ = 0;
  playbackSampleRate_ = Config::AUDIO_SAMPLE_RATE;
  playbackChannels_ = Config::AUDIO_CHANNELS;
  playbackEof_ = false;
  if (playbackBuffer_) (void)xStreamBufferReset(playbackBuffer_);
  (void)fillPlaybackBuffer();

  bool committed = false;
  {
    StateLock lock(gState);
    if (lock.ok() && gState.storageReady && !gState.usbAudioActive && !gState.recording) {
      playing_ = true;
      gState.playing = true;
      gState.playbackPaused = false;
      gState.playbackPositionMs = 0;
      gState.playbackDurationMs = 0;
      gState.lastAudioFile = path;
      committed = true;
    }
  }

  if (!committed) {
    playing_ = false;
    playbackRemaining_ = 0;
    closePlaybackDecoder();
    SpiLock spiLock(pdMS_TO_TICKS(50));
    if (spiLock.ok() && playFile_) playFile_.close();
    xSemaphoreGive(mutex_);
    return false;
  }

  xSemaphoreGive(mutex_);
  logEvent("PLAY_START", path);
  return true;
}

void AudioManager::stopPlayback() {
  if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE)
    return;
  playing_ = false;
  playbackPaused_ = false;
  playbackRemaining_ = 0;
  closePlaybackDecoder();
  if (playbackBuffer_) (void)xStreamBufferReset(playbackBuffer_);
  {
    SpiLock spiLock(pdMS_TO_TICKS(20));
    if (spiLock.ok() && playFile_) playFile_.close();
  }
  {
    StateLock lock(gState);
    if (lock.ok()) gState.playing = false;
  }
  xSemaphoreGive(mutex_);
  logEvent("PLAY_STOP");
}

void AudioManager::setVolume(uint8_t percent) {
  volume_ = constrain(percent, 0, 100);
  (void)codec.setVolumePercent(volume_);
  StateLock lock(gState);
  if (lock.ok()) gState.volume = volume_;
}

bool AudioManager::setClassDConfig(bool enabled, uint8_t boostLevel) {
  if (!initialized_) return false;
  if (!codec.setClassDConfig(enabled, boostLevel)) return false;
  return codec.setVolumePercent(volume_);
}

esp_err_t AudioManager::usbOutputCallback(uint8_t* data, size_t len, void* /*ctx*/) {
  if (!instance_ || !data || !len || !instance_->i2sMutex_)
    return ESP_ERR_INVALID_ARG;

  instance_->updateUsbSampleRate(len);
  size_t written = 0;
  if (xSemaphoreTake(instance_->i2sMutex_, pdMS_TO_TICKS(20)) != pdTRUE)
    return ESP_ERR_TIMEOUT;

  const esp_err_t err = i2s_write(AUDIO_I2S_PORT, data, len, &written,
                                   pdMS_TO_TICKS(20));
  bool captureDropped = false;
  if (err == ESP_OK && written == len &&
      instance_->captureUsbRecord_ && instance_->usbRecordBuffer_) {
    const size_t queued = xStreamBufferSend(
        instance_->usbRecordBuffer_, data, len, 0);
    captureDropped = queued != len;
  }
  xSemaphoreGive(instance_->i2sMutex_);
  if (err == ESP_OK && written == len) {
    instance_->queueUsbAecReference(data, len, instance_->usbSampleRate_);
  }
  if (err != ESP_OK || written != len || captureDropped) {
    StateLock lock(gState);
    if (lock.ok()) gState.audioDrops++;
    if (err != ESP_OK || written != len) {
      return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    }
  }

  instance_->updateAudioLevel(data, len);
  instance_->lastUsbAudioMs_ = millis();
  StateLock lock(gState);
  if (lock.ok()) gState.usbAudioActive = true;
  return ESP_OK;
}

esp_err_t AudioManager::usbInputCallback(uint8_t* data, size_t len,
                                         size_t* bytesRead, void* /*ctx*/) {
  if (!instance_ || !data || !bytesRead || !len || !instance_->i2sMutex_)
    return ESP_ERR_INVALID_ARG;

  *bytesRead = 0;

  if (instance_->usbPlaybackTransport_ && instance_->usbTransportBuffer_) {
    const size_t got = xStreamBufferReceive(
        instance_->usbTransportBuffer_, data, len, 0);
    if (got < len) memset(data + got, 0, len - got);
    *bytesRead = len;
  } else if (instance_->captureWm8962Mic_ && instance_->usbMicBuffer_) {
    const size_t got = xStreamBufferReceive(
        instance_->usbMicBuffer_, data, len, 0);
    if (got < len) memset(data + got, 0, len - got);
    *bytesRead = len;
  } else if (!instance_->usbMonitor_) {
    memset(data, 0, len);
    *bytesRead = len;
  } else {
    if (xSemaphoreTake(instance_->i2sMutex_, pdMS_TO_TICKS(20)) != pdTRUE)
      return ESP_ERR_TIMEOUT;

    size_t got = 0;
    const esp_err_t err = i2s_read(AUDIO_I2S_PORT, data, len, &got,
                                   pdMS_TO_TICKS(20));
    xSemaphoreGive(instance_->i2sMutex_);
    if (err != ESP_OK) {
      StateLock lock(gState);
      if (lock.ok()) gState.audioDrops++;
      return err;
    }
    *bytesRead = got;
  }
  instance_->lastUsbAudioMs_ = millis();
  StateLock lock(gState);
  if (lock.ok()) gState.usbAudioActive = true;
  return ESP_OK;
}

void AudioManager::usbMuteCallback(uint32_t mute, void* /*ctx*/) {
  if (!instance_) return;
  if (mute) {
    instance_->preMuteVolume_ = instance_->volume_ ? instance_->volume_ : 70;
    instance_->usbMuted_ = true;
    instance_->setVolume(0);
  } else {
    instance_->usbMuted_ = false;
    instance_->setVolume(instance_->preMuteVolume_);
  }
  StateLock lock(gState);
  if (lock.ok()) gState.usbMuted = instance_->usbMuted_;
}

void AudioManager::usbVolumeCallback(uint32_t volume, void* /*ctx*/) {
  if (!instance_) return;
  const uint8_t percent = static_cast<uint8_t>(constrain(volume, 0U, 100U));
  instance_->usbVolume_ = percent;
  instance_->setVolume(percent);
  instance_->usbVolumeDirty_ = true;
  instance_->usbVolumeDirtyMs_ = millis();
  StateLock lock(gState);
  if (lock.ok()) {
    gState.usbVolume = percent;
    gState.usbMuted = false;
  }
}

bool AudioManager::usbStart() {
  if (!initialized_) return false;

  uac_device_config_t cfg{};
  cfg.skip_tinyusb_init = false;
  cfg.output_cb = usbOutputCallback;
  cfg.input_cb = usbInputCallback;
  cfg.set_mute_cb = usbMuteCallback;
  cfg.set_volume_cb = usbVolumeCallback;
  cfg.cb_ctx = this;

  const esp_err_t err = uac_device_init(&cfg);
  const bool ok = err == ESP_OK;
  StateLock lock(gState);
  if (lock.ok()) {
    gState.usbAudioReady = ok;
    if (!ok) gState.lastError = "USB Audio Class init failed";
  }
  return ok;
}

void AudioManager::task() {
  if (usbVolumeDirty_ && millis() - usbVolumeDirtyMs_ >= Config::USB_VOLUME_PERSIST_DELAY_MS) {
    const uint8_t volume = usbVolume_;
    RuntimeConfig candidate{};
    const bool snapshotOk = configSnapshot(candidate);
    if (snapshotOk) candidate.volume = volume;
    if (snapshotOk && configCommit(candidate)) {
      usbVolumeDirty_ = false;
    } else {
      StateLock lock(gState);
      if (lock.ok()) gState.lastError = "USB volume NVS save failed";
      usbVolumeDirty_ = false;
    }
  }

  if (millis() - lastUsbAudioMs_ > 500) {
    StateLock lock(gState);
    if (lock.ok()) gState.usbAudioActive = false;
  }

  if (!initialized_ || !mutex_ ||
      xSemaphoreTake(mutex_, 0) != pdTRUE)
    return;

  if (!playing_ && playFile_) {
    SpiLock spiLock(pdMS_TO_TICKS(20));
    if (spiLock.ok()) playFile_.close();
  }

  if (playing_) {
    if (playbackPaused_) {
      xSemaphoreGive(mutex_);
      return;
    }

    (void)fillPlaybackBuffer();
    bool done = playbackEof_ && xStreamBufferBytesAvailable(playbackBuffer_) == 0;
    if (!done) {
      if (!playDecodedBuffer()) {
        StateLock lock(gState);
        if (lock.ok()) {
          gState.audioDrops++;
          gState.lastError = "Audio decoder/I2S playback failed";
        }
        done = true;
      }
    }

    {
      StateLock lock(gState);
      if (lock.ok()) gState.playbackPositionMs = playbackPositionMs_;
    }

    if (done) {
      playing_ = false;
      playbackRemaining_ = 0;
      closePlaybackDecoder();
      {
        SpiLock spiLock(pdMS_TO_TICKS(50));
        if (spiLock.ok() && playFile_) playFile_.close();
      }
      StateLock lock(gState);
      if (lock.ok()) gState.playing = false;
      if (queueCount_) {
        xSemaphoreGive(mutex_);
        (void)playNextQueued();
        return;
      }
    }
  }

  bool rec = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) {
      xSemaphoreGive(mutex_);
      return;
    }
    rec = gState.recording;
  }
  if (!rec) {
    if (recordSource_ == Config::AUDIO_SOURCE_WM8962_MIC &&
        captureWm8962Mic_ && !playing_ && usbMicBuffer_ &&
        xStreamBufferSpacesAvailable(usbMicBuffer_) >= Config::AUDIO_IO_BYTES) {
      uint8_t micBuffer[Config::AUDIO_IO_BYTES];
      size_t micGot = 0;
      bool i2sLocked = i2sMutex_ &&
                       xSemaphoreTake(i2sMutex_, pdMS_TO_TICKS(5)) == pdTRUE;
      const esp_err_t micErr = i2sLocked
          ? i2s_read(AUDIO_I2S_PORT, micBuffer, sizeof(micBuffer), &micGot,
                     pdMS_TO_TICKS(5))
          : ESP_ERR_TIMEOUT;
      if (i2sLocked) xSemaphoreGive(i2sMutex_);
      if (micErr == ESP_OK && micGot > 0) {
        updateAudioLevel(micBuffer, micGot);
        if (voxEnabled_ && voxAdaptMs_ &&
            millis() - voxAdaptStartedMs_ <= voxAdaptMs_) {
          StateLock noiseLock(gState);
          if (noiseLock.ok() && gState.audioRms < max(0.02f, voxThreshold_)) {
            voxNoiseFloor_ = voxNoiseFloor_ == 0.0f
                ? gState.audioRms
                : (voxNoiseFloor_ * 0.95f + gState.audioRms * 0.05f);
            voxThreshold_ = max(voxThreshold_, voxNoiseFloor_ * 2.5f);
          }
        }
        if (xStreamBufferSend(usbMicBuffer_, micBuffer, micGot, 0) != micGot) {
          StateLock lock(gState);
          if (lock.ok()) ++gState.audioDrops;
        }
      }
    }
    if (recordFile_) {
      (void)finalizeWav();
    } else if (!recordPath_.isEmpty()) {
      discardRecordingFile();
    }
    const bool vox = voxEnabled_;
    const float rms = [&]() {
      StateLock lock(gState);
      return lock.ok() ? gState.audioRms : 0.0f;
    }();
    xSemaphoreGive(mutex_);
    if (vox && rms >= voxThreshold_) (void)startRecording();
    return;
  }

  if (!recordingPaused_ &&
      millis() - recordStartedMs_ >= min(Config::RECORD_SPLIT_SECONDS, Config::RECORD_MAX_SECONDS) * 1000UL) {
    xSemaphoreGive(mutex_);
    (void)splitRecording();
    return;
  }

  if (recordingPaused_) {
    xSemaphoreGive(mutex_);
    return;
  }

  uint8_t buffer[Config::AUDIO_IO_BYTES];

  if (recordSource_ == Config::AUDIO_SOURCE_USB) {
    const size_t got = usbRecordBuffer_
        ? xStreamBufferReceive(usbRecordBuffer_, buffer, sizeof(buffer), 0)
        : 0;
    if (got > 0 && recordFile_) updateAudioLevel(buffer, got);
    if (got > 0 && recordFile_ && !writeRecordingData(buffer, got)) {
      captureUsbRecord_ = false;
      StateLock lock(gState);
      if (lock.ok()) gState.recording = false;
      (void)finalizeWav();
    }
    xSemaphoreGive(mutex_);
    return;
  }

  size_t got = 0;
  bool i2sLocked = i2sMutex_ &&
                   xSemaphoreTake(i2sMutex_, pdMS_TO_TICKS(5)) == pdTRUE;
  const esp_err_t readErr = i2sLocked
      ? i2s_read(AUDIO_I2S_PORT, buffer, sizeof(buffer), &got, pdMS_TO_TICKS(5))
      : ESP_ERR_TIMEOUT;
  if (i2sLocked) xSemaphoreGive(i2sMutex_);

  if (readErr == ESP_OK && got > 0 && recordFile_) {
    updateAudioLevel(buffer, got);
    if (usbMicBuffer_) {
      const size_t queued = xStreamBufferSend(usbMicBuffer_, buffer, got, 0);
      if (queued != got) {
        StateLock lock(gState);
        if (lock.ok()) gState.audioDrops++;
      }
    }
    if (!writeRecordingData(buffer, got)) {
      captureWm8962Mic_ = false;
      StateLock lock(gState);
      if (lock.ok()) gState.recording = false;
      (void)finalizeWav();
    }
  }

  bool voxStop = false;
  if (voxEnabled_ && !recordingPaused_) {
    const uint32_t now = millis();
    StateLock lock(gState);
    if (lock.ok()) {
      if (gState.audioRms >= voxThreshold_) {
        voxLastVoiceMs_ = now;
      } else if (voxLastVoiceMs_ != 0 && now - voxLastVoiceMs_ >= voxHangMs_) {
        voxStop = true;
      }
    }
  }
  xSemaphoreGive(mutex_);
  if (voxStop) (void)stopRecording();
}
