#pragma once
#include <Arduino.h>
#include "Config.h"
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <usb_device_uac.h>
#include <esp_aec.h>
#include <esp_heap_caps.h>

class AudioManager {
public:
  bool begin();
  void task();
  bool startRecording();
  bool stopRecording();
  bool pauseRecording(bool paused);
  bool splitRecording();
  bool playFile(const String& path);
  void stopPlayback();
  bool pausePlayback(bool paused);
  bool seekPlaybackMs(uint32_t positionMs);
  bool enqueueFile(const String& path);
  void clearQueue();
  bool setVox(bool enabled, float threshold = 0.08f, uint32_t hangMs = 700);
  void setVolume(uint8_t percent);
  bool usbStart();
  bool setRecordSource(uint8_t source);
  bool setUsbMonitor(bool enabled);
  bool setUsbPlaybackTransport(bool enabled);
  bool setLoopback(bool enabled);
  bool setAec(bool enabled);
  bool aecEnabled() const { return aecEnabled_; }
  uint32_t usbSampleRate() const { return usbSampleRate_; }
  bool playTone(uint16_t frequencyHz, uint16_t durationMs, uint8_t percent = 35);
  bool captureVoiceFrame(uint8_t* out, size_t capacity, size_t& written);
  bool playVoiceFrame(const uint8_t* data, size_t len);
  bool usbMonitor() const { return usbMonitor_; }
  bool loopback() const { return loopback_; }
  uint8_t recordSource() const { return recordSource_; }

private:
  File recordFile_;
  File playFile_;
  String recordPath_;

  uint32_t recordedBytes_ = 0;
  uint32_t playbackRemaining_ = 0;
  uint32_t recordStartedMs_ = 0;
  bool initialized_ = false;
  bool playing_ = false;
  uint8_t volume_ = 70;
  uint8_t preMuteVolume_ = 70;
  volatile uint32_t lastUsbAudioMs_ = 0;
  volatile uint8_t recordSource_ = Config::AUDIO_SOURCE_WM8962_MIC;
  volatile bool captureUsbRecord_ = false;
  volatile bool captureWm8962Mic_ = false;
  volatile bool usbMonitor_ = false;
  volatile bool loopback_ = false;
  volatile bool usbMuted_ = false;
  volatile uint8_t usbVolume_ = 100;
  volatile bool usbVolumeDirty_ = false;
  volatile uint32_t usbVolumeDirtyMs_ = 0;
  StreamBufferHandle_t usbRecordBuffer_ = nullptr;
  StreamBufferHandle_t usbMicBuffer_ = nullptr;
  StreamBufferHandle_t playbackBuffer_ = nullptr;
  StreamBufferHandle_t usbTransportBuffer_ = nullptr;
  StreamBufferHandle_t aecRefBuffer_ = nullptr;
  aec_handle_t aec_ = nullptr;
  bool aecEnabled_ = Config::AEC_ENABLED_BY_DEFAULT;
  uint16_t aecFrameSize_ = 0;
  uint32_t usbSampleRate_ = Config::AUDIO_SAMPLE_RATE;
  uint32_t usbRatePhase_ = 0;
  int16_t* aecMic_ = nullptr;
  int16_t* aecRef_ = nullptr;
  int16_t* aecOut_ = nullptr;
  alignas(4) int16_t aecMicFallback_[Config::AEC_FRAME_SAMPLES] = {};
  alignas(4) int16_t aecRefFallback_[Config::AEC_FRAME_SAMPLES] = {};
  alignas(4) int16_t aecOutFallback_[Config::AEC_FRAME_SAMPLES] = {};
  uint8_t* aecRefStorage_ = nullptr;
  alignas(4) uint8_t aecRefStorageFallback_[Config::USB_AEC_REFERENCE_BYTES] = {};
  StaticStreamBuffer_t aecRefBufferStatic_{};
  volatile bool usbPlaybackTransport_ = false;
  void* simpleDecoder_ = nullptr;
  uint8_t decoderType_ = 0;
  bool compressedPlayback_ = false;
  bool playbackEof_ = false;
  uint8_t playbackInput_[4096] = {};
  uint8_t playbackPcm_[16384] = {};
  bool playbackPaused_ = false;
  uint32_t playbackPositionMs_ = 0;
  uint32_t playbackSampleRate_ = Config::AUDIO_SAMPLE_RATE;
  uint16_t playbackChannels_ = Config::AUDIO_CHANNELS;
  uint32_t playbackTotalBytes_ = 0;
  String queue_[16];
  uint8_t queueHead_ = 0, queueTail_ = 0, queueCount_ = 0;
  bool voxEnabled_ = false;
  float voxThreshold_ = 0.08f;
  uint32_t voxHangMs_ = 700;
  uint32_t voxLastVoiceMs_ = 0;
  bool recordingPaused_ = false;
  uint16_t recordingPart_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t i2sMutex_ = nullptr;

  bool initCodec();
  bool initI2S();
  bool routeInput(uint8_t source);
  void updateAudioLevel(const uint8_t* data, size_t len);
  bool writeWavHeader(File& f, uint32_t dataBytes);
  bool finalizeWav();
  bool writeRecordingData(const uint8_t* data, size_t len);
  bool flushUsbRecordingBuffer();
  bool openRecordingPart();
  bool openPlaybackDecoder(const String& path);
  void closePlaybackDecoder();
  bool fillPlaybackBuffer();
  bool playDecodedBuffer();
  bool playNextQueued();
  void discardRecordingFile();
  bool initAec();
  void deinitAec();
  void updateUsbSampleRate(size_t len);
  void queueUsbAecReference(const uint8_t* data, size_t len, uint32_t inputRate = Config::AUDIO_SAMPLE_RATE);
  static esp_err_t usbOutputCallback(uint8_t* data, size_t len, void* ctx);
  static esp_err_t usbInputCallback(uint8_t* data, size_t len, size_t* bytesRead, void* ctx);
  static void usbMuteCallback(uint32_t mute, void* ctx);
  static void usbVolumeCallback(uint32_t volume, void* ctx);
  static AudioManager* instance_;
};
