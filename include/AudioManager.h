#pragma once
#include <Arduino.h>
#include <FS.h>
#include <BluetoothA2DPSink.h>

class AudioManager {
public:
  bool begin();
  void task();
  bool startRecording();
  void stopRecording();
  bool playFile(const String& path);
  void stopPlayback();
  void setVolume(uint8_t percent);
  bool btStart();

private:
  File recordFile_;
  File playFile_;
  uint32_t recordedBytes_ = 0;
  uint32_t recordStartedMs_ = 0;
  bool initialized_ = false;
  bool playing_ = false;
  bool btStarted_ = false;
  uint8_t volume_ = 70;
  BluetoothA2DPSink a2dp_;

  bool initCodec();
  bool initI2S();
  bool readWavHeader(File& f, uint32_t& dataOffset, uint32_t& dataBytes,
                     uint16_t& channels, uint32_t& sampleRate,
                     uint16_t& bitsPerSample);
  void writeWavHeader(File& f, uint32_t dataBytes);
  void finalizeWav();
  static void btDataCallback(const uint8_t* data, uint32_t len);
  static void btConnectionCallback(esp_a2d_connection_state_t state, void* ptr);
  static AudioManager* instance_;
};
