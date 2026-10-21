#pragma once

// ============================================================
//  ESP AUDIO DECODER
//  arduino-audio-tools AudioDecoder backed by Espressif's
//  esp_audio_codec "simple decoder" (the same decoder used by
//  Basmio). Handles MP3, AAC/HE-AAC, M4A, FLAC, OGG (Vorbis/Opus)
//  and WAV with internal frame parsing and resync, so it accepts
//  arbitrary-sized chunks from SD files or network streams.
//
//  Output is always 16-bit interleaved stereo; only the sample
//  rate follows the source.
// ============================================================

#include "AudioTools.h"
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

class EspAudioDecoder : public AudioDecoder {
 public:
  EspAudioDecoder() = default;
  ~EspAudioDecoder() override;

  /// Select the container/codec. Must be called before begin().
  bool setCodec(CodecHint codec);
  CodecHint codec() const { return selectedCodec; }

  bool begin() override;
  void end() override;
  size_t write(const uint8_t* data, size_t len) override;
  operator bool() override { return handle != nullptr; }

  /// The stream itself defines the format; ignore stale info pushed in by
  /// EncodedAudioStream so listeners only ever see what was decoded.
  void setAudioInfo(AudioInfo from) override { (void)from; }

  /// Signal end of stream so parsers with internal caches (FLAC) flush.
  void finish();

  uint32_t errorCount() const { return decodeErrors; }
  uint32_t droppedBytes() const { return bytesDropped; }

 private:
  static bool registerCodecs();
  bool allocBuffers();
  void freeBuffers();
  void skipId3Header();
  void decodeBuffered(bool eos);
  void emitPcm(const uint8_t* pcm, size_t bytes, const esp_audio_simple_dec_info_t& decInfo);

  CodecHint selectedCodec = CODEC_UNKNOWN;
  esp_audio_simple_dec_handle_t handle = nullptr;

  uint8_t* inBuf = nullptr;
  size_t inLen = 0;
  uint8_t* outBuf = nullptr;
  size_t outCap = 0;
  int16_t* pcmBuf = nullptr;
  size_t pcmCap = 0;

  AudioInfo emittedInfo = AudioInfo(0, 0, 0);
  bool headerChecked = false;
  size_t id3Remaining = 0;

  uint32_t decodeErrors = 0;
  uint32_t bytesDropped = 0;
};
