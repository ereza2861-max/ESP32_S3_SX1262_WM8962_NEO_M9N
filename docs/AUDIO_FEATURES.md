# Audio feature contract

This firmware uses the ESP32-S3 + WM8960 clock domain at 44.1 kHz.

Implemented in this patch:
- MP3 playback through `espressif/esp_audio_codec` simple decoder.
- OGG/Opus playback through the simple decoder. `.OPUS` files must be Ogg/Opus containers; raw Opus packets are not self-framing.
- WAV playback through the simple decoder, including PCM and IMA-ADPCM WAV where supported by the codec component.
- SD playback pre-buffering.
- playback pause/resume, PCM WAV seek, bounded queue, and automatic queue advance.
- recording pause/resume and automatic file splitting.
- VOX start/stop with configurable threshold/hang.
- SD playback -> USB microphone transport, using the USB transport buffer.
- USB UAC fixed 44.1 kHz configuration with synchronous feedback endpoint enabled by `usb_device_uac`.
- narrow-band Voice-over-LoRa using 8 kHz G.711 μ-law frames. This is deliberately bounded by the existing LoRa duty-cycle budget; continuous full-duplex voice is not possible under a 1% duty-cycle constraint.

Important limitations:
- `usb_device_uac` 1.3.1 exposes synchronous feedback but explicitly does not implement host-selected dynamic sample-rate switching. Therefore this patch fixes the previously inconsistent 48 kHz USB descriptor vs 44.1 kHz I2S clock, but does not pretend to provide dynamic sample-rate negotiation. A true dynamic negotiation implementation requires making the UAC descriptor/component a project-local custom TinyUSB UAC function and reprogramming the WM8960/ESP32 I2S clock at every accepted SET_CUR sample-rate request.
- ESP-SR is added as a dependency for the AEC/NS integration stage, but AEC/NS is not silently enabled on the 44.1 kHz recording path. ESP-SR AEC currently operates at 16 kHz and requires a synchronized playback reference. Enabling it without a real reference/resampler would create worse audio and false confidence. The WM8960 hardware noise gate/ALC remains active for the microphone source.
