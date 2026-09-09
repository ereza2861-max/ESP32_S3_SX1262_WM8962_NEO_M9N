#include "AudioManager.h"
#include "BoardConfig.h"
#include "Config.h"
#include "AppState.h"
#include <Wire.h>
#include <SD.h>
#include <driver/i2s.h>
#include <SparkFun_WM8960_Arduino_Library.h>

static const i2s_port_t AUDIO_I2S_PORT = I2S_NUM_0;
static WM8960 codec;
AudioManager* AudioManager::instance_ = nullptr;

static void configureCodecForI2S() {
  codec.enableVREF();
  codec.enableVMID();

  // Mic/line input -> ADC.
  codec.enableLMIC();
  codec.enableRMIC();
  codec.connectLMN1();
  codec.connectRMN1();
  codec.disableLINMUTE();
  codec.disableRINMUTE();
  codec.setLINVOLDB(0.0);
  codec.setRINVOLDB(0.0);
  codec.setLMICBOOST(WM8960_MIC_BOOST_GAIN_0DB);
  codec.setRMICBOOST(WM8960_MIC_BOOST_GAIN_0DB);
  codec.connectLMIC2B();
  codec.connectRMIC2B();
  codec.enableAINL();
  codec.enableAINR();

  // DAC -> output mixer / headphone path.
  codec.disableLB2LO();
  codec.disableRB2RO();
  codec.enableLD2LO();
  codec.enableRD2RO();
  codec.setLB2LOVOL(WM8960_OUTPUT_MIXER_GAIN_NEG_21DB);
  codec.setRB2ROVOL(WM8960_OUTPUT_MIXER_GAIN_NEG_21DB);
  codec.enableLOMIX();
  codec.enableROMIX();

  // 24 MHz MCLK on PCB -> WM8960 PLL -> 44.1 kHz audio clock.
  codec.enablePLL();
  codec.setPLLPRESCALE(WM8960_PLLPRESCALE_DIV_2);
  codec.setSMD(WM8960_PLL_MODE_FRACTIONAL);
  codec.setCLKSEL(WM8960_CLKSEL_PLL);
  codec.setSYSCLKDIV(WM8960_SYSCLK_DIV_BY_2);
  codec.setBCLKDIV(4);
  codec.setDCLKDIV(WM8960_DCLKDIV_16);
  codec.setPLLN(7);
  codec.setPLLK(0x86, 0xC2, 0x26);

  // Codec is I2S master; ESP32 is I2S slave.
  codec.enableMasterMode();
  codec.setALRCGPIO();

  codec.enableAdcLeft();
  codec.enableAdcRight();
  codec.enableDacLeft();
  codec.enableDacRight();
  codec.disableDacMute();

  codec.enableHeadphones();
  codec.enableOUT3MIX();
  codec.setHeadphoneVolumeDB(0.0);
}

bool AudioManager::initCodec() {
  Wire.begin(Board::I2C_SDA, Board::I2C_SCL, 400000);
  if (!codec.begin()) return false;
  configureCodecForI2S();
  return true;
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

bool AudioManager::begin() {
  instance_ = this;
  if (!initCodec()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "WM8960 init failed";
    return false;
  }
  if (!initI2S()) {
    StateLock lock(gState);
    if (lock.ok()) gState.lastError = "I2S init failed";
    return false;
  }

  initialized_ = true;
  StateLock lock(gState);
  if (lock.ok()) gState.codecReady = true;
  return true;
}

void AudioManager::writeWavHeader(File& f, uint32_t dataBytes) {
  if (!f) return;

  const uint32_t byteRate = Config::AUDIO_SAMPLE_RATE * 4;
  const uint16_t blockAlign = 4;
  const uint16_t bits = 16;

  uint8_t h[44] = {
    'R','I','F','F',0,0,0,0,'W','A','V','E',
    'f','m','t',' ',16,0,0,0,1,0,2,0,
    0,0,0,0,0,0,0,0,4,0,16,0,
    'd','a','t','a',0,0,0,0
  };

  uint32_t riff = 36 + dataBytes;
  memcpy(h + 4, &riff, 4);
  uint32_t sr = Config::AUDIO_SAMPLE_RATE;
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &blockAlign, 2);
  memcpy(h + 40, &dataBytes, 4);

  f.seek(0);
  f.write(h, sizeof(h));
}

bool AudioManager::startRecording() {
  if (!initialized_ || playing_) return false;

  StateLock lock(gState);
  if (!lock.ok() || gState.recording || gState.btConnected) return false;

  if (!SD.exists("/REC")) SD.mkdir("/REC");
  String path = "/REC/REC_" + String(millis()) + ".WAV";

  recordFile_ = SD.open(path, FILE_WRITE);
  if (!recordFile_) {
    gState.lastError = "WAV create failed";
    return false;
  }

  uint8_t zero[44] = {};
  if (recordFile_.write(zero, sizeof(zero)) != sizeof(zero)) {
    recordFile_.close();
    gState.lastError = "WAV header reserve failed";
    return false;
  }

  recordedBytes_ = 0;
  recordStartedMs_ = millis();
  gState.recording = true;
  gState.lastAudioFile = path;
  return true;
}

void AudioManager::finalizeWav() {
  if (!recordFile_) return;
  writeWavHeader(recordFile_, recordedBytes_);
  recordFile_.flush();
  recordFile_.close();
}

void AudioManager::stopRecording() {
  StateLock lock(gState);
  if (!lock.ok() || !gState.recording) return;
  gState.recording = false;
  finalizeWav();
}

bool AudioManager::readWavHeader(File& f, uint32_t& dataOffset,
                                 uint32_t& dataBytes, uint16_t& channels,
                                 uint32_t& sampleRate, uint16_t& bitsPerSample) {
  uint8_t h[44];
  if (!f || f.read(h, sizeof(h)) != sizeof(h)) return false;
  if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
    return false;
  if (memcmp(h + 12, "fmt ", 4) != 0 || memcmp(h + 36, "data", 4) != 0)
    return false;

  channels = h[22] | (static_cast<uint16_t>(h[23]) << 8);
  memcpy(&sampleRate, h + 24, 4);
  bitsPerSample = h[34] | (static_cast<uint16_t>(h[35]) << 8);
  memcpy(&dataBytes, h + 40, 4);
  dataOffset = 44;

  return channels == 2 && sampleRate == Config::AUDIO_SAMPLE_RATE &&
         bitsPerSample == 16 && dataBytes > 0;
}

bool AudioManager::playFile(const String& path) {
  if (!initialized_ || path.length() > Config::MAX_PATH ||
      path.indexOf("..") >= 0 || !path.startsWith("/"))
    return false;

  {
    StateLock lock(gState);
    if (!lock.ok() || gState.btConnected) return false;
  }

  stopPlayback();
  playFile_ = SD.open(path, FILE_READ);
  if (!playFile_) return false;

  uint32_t offset, bytes;
  uint16_t channels, bits;
  uint32_t rate;
  if (!readWavHeader(playFile_, offset, bytes, channels, rate, bits)) {
    playFile_.close();
    return false;
  }

  playing_ = true;
  StateLock lock(gState);
  if (lock.ok()) {
    gState.playing = true;
    gState.lastAudioFile = path;
  }
  return true;
}

void AudioManager::stopPlayback() {
  playing_ = false;
  if (playFile_) playFile_.close();
  StateLock lock(gState);
  if (lock.ok()) gState.playing = false;
}

void AudioManager::setVolume(uint8_t percent) {
  volume_ = constrain(percent, 0, 100);
  const float db = -74.0f + (80.0f * volume_ / 100.0f);
  codec.setHeadphoneVolumeDB(db);
  StateLock lock(gState);
  if (lock.ok()) gState.volume = volume_;
}

void AudioManager::btDataCallback(const uint8_t* data, uint32_t len) {
  if (!instance_ || !data || !len) return;

  // Callback is intentionally non-blocking: the A2DP library owns the
  // Bluetooth task, and I2S TX is already DMA buffered.
  size_t written = 0;
  if (i2s_write(AUDIO_I2S_PORT, data, len, &written, 0) != ESP_OK ||
      written != len) {
    StateLock lock(gState);
    if (lock.ok()) gState.audioDrops++;
  }
}

void AudioManager::btConnectionCallback(esp_a2d_connection_state_t state,
                                         void* /*ptr*/) {
  StateLock lock(gState);
  if (lock.ok()) {
    gState.btConnected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  }
}

bool AudioManager::btStart() {
  if (!initialized_ || btStarted_) return btStarted_;

  a2dp_.set_stream_reader(btDataCallback, false);
  a2dp_.set_on_connection_state_changed(btConnectionCallback);
  a2dp_.set_task_core(0);
  a2dp_.set_task_priority(4);
  a2dp_.start(Config::BT_NAME);

  btStarted_ = true;
  StateLock lock(gState);
  if (lock.ok()) gState.btStarted = true;
  return true;
}

void AudioManager::task() {
  if (!initialized_) return;

  if (playing_) {
    if (!playFile_) {
      stopPlayback();
      return;
    }

    uint8_t buffer[Config::AUDIO_IO_BYTES];
    size_t n = playFile_.read(buffer, sizeof(buffer));
    if (!n) {
      stopPlayback();
      return;
    }

    size_t written = 0;
    if (i2s_write(AUDIO_I2S_PORT, buffer, n, &written, pdMS_TO_TICKS(20)) != ESP_OK ||
        written != n) {
      StateLock lock(gState);
      if (lock.ok()) gState.audioDrops++;
      stopPlayback();
      return;
    }
  }

  bool rec = false;
  {
    StateLock lock(gState);
    if (!lock.ok()) return;
    rec = gState.recording;
  }
  if (!rec) return;

  if (millis() - recordStartedMs_ >= Config::RECORD_MAX_SECONDS * 1000UL) {
    stopRecording();
    return;
  }

  uint8_t buffer[Config::AUDIO_IO_BYTES];
  size_t got = 0;
  if (i2s_read(AUDIO_I2S_PORT, buffer, sizeof(buffer), &got, pdMS_TO_TICKS(5)) == ESP_OK &&
      got > 0 && recordFile_) {
    const size_t written = recordFile_.write(buffer, got);
    recordedBytes_ += written;
    if (written != got) {
      StateLock lock(gState);
      if (lock.ok()) gState.audioDrops++;
    }
  }
}
