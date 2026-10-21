#pragma once

// ============================================================
//  ESP AUDIO DECODER
//  Wrapper around Espressif's esp_audio_codec "simple decoder"
//  (the same decoder Basmio uses). Handles MP3, AAC/HE-AAC, M4A,
//  FLAC, OGG (Vorbis/Opus) and WAV with internal frame parsing
//  and resync, so it accepts arbitrary-sized chunks from SD files
//  or network streams.
//
//  Decoded audio is always delivered as 16-bit interleaved stereo;
//  only the sample rate follows the source.
// ============================================================

#include <stddef.h>
#include <stdint.h>

#include "esp_audio_simple_dec.h"

enum CodecHint {
  CODEC_UNKNOWN,
  CODEC_MP3,
  CODEC_AAC,
  CODEC_M4A,
  CODEC_FLAC,
  CODEC_OGG,
  CODEC_WAV
};

class EspAudioDecoder {
 public:
  using PcmSink = void (*)(const int16_t* stereo, size_t frames, uint32_t sampleRate);

  EspAudioDecoder() = default;
  ~EspAudioDecoder();
  EspAudioDecoder(const EspAudioDecoder&) = delete;
  EspAudioDecoder& operator=(const EspAudioDecoder&) = delete;

  void setSink(PcmSink sink) { pcmSink = sink; }

  bool begin(CodecHint codec);
  void end();
  void write(const uint8_t* data, size_t len);
  bool active() const { return handle != nullptr; }
  CodecHint codec() const { return selectedCodec; }

  /// Signal end of stream so parsers with internal caches (FLAC) flush.
  void finish();

  uint32_t sampleRate() const { return lastSampleRate; }
  uint32_t errorCount() const { return decodeErrors; }
  uint32_t droppedBytes() const { return bytesDropped; }

  static const char* codecName(CodecHint codec);

 private:
  static void registerCodecs();
  bool allocBuffers();
  void freeBuffers();
  void skipId3Header();
  void decodeBuffered(bool eos);
  void emitPcm(const uint8_t* pcm, size_t bytes, const esp_audio_simple_dec_info_t& decInfo);

  PcmSink pcmSink = nullptr;
  CodecHint selectedCodec = CODEC_UNKNOWN;
  esp_audio_simple_dec_handle_t handle = nullptr;

  uint8_t* inBuf = nullptr;
  size_t inLen = 0;
  uint8_t* outBuf = nullptr;
  size_t outCap = 0;
  int16_t* pcmBuf = nullptr;
  size_t pcmCap = 0;

  uint32_t lastSampleRate = 0;
  uint8_t lastChannels = 0;
  bool headerChecked = false;
  size_t id3Remaining = 0;

  uint32_t decodeErrors = 0;
  uint32_t bytesDropped = 0;
};
