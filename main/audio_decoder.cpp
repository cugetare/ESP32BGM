// ============================================================
//  ESP AUDIO DECODER (esp_audio_codec simple decoder wrapper)
// ============================================================
#include "audio_decoder.h"

#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_aac_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"

static const char* TAG = "decoder";

static const size_t INPUT_BUFFER_SIZE  = 16 * 1024;  // same as Basmio
static const size_t OUTPUT_BUFFER_SIZE = 8 * 1024;   // grows on demand

static esp_audio_simple_dec_type_t simpleTypeFor(CodecHint codec) {
  switch (codec) {
    case CODEC_MP3:  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    case CODEC_AAC:  return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case CODEC_M4A:  return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case CODEC_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case CODEC_OGG:  return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    case CODEC_WAV:  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    case CODEC_UNKNOWN: break;
  }
  return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

// Reads one sample of any supported width and returns its top 16 bits.
static inline int16_t sampleAt(const uint8_t* p, size_t bytesPerSample) {
  switch (bytesPerSample) {
    case 1: return (int16_t)(((int)p[0] - 128) << 8);  // 8-bit PCM is unsigned
    case 2: return (int16_t)(p[0] | (p[1] << 8));
    case 3: return (int16_t)(p[1] | (p[2] << 8));
    default: return (int16_t)(p[bytesPerSample - 2] | (p[bytesPerSample - 1] << 8));
  }
}

// Large work buffers go to PSRAM when available.
static void* bufferAlloc(size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(size);
}

static void* bufferRealloc(void* ptr, size_t size) {
  void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : realloc(ptr, size);
}

const char* EspAudioDecoder::codecName(CodecHint codec) {
  switch (codec) {
    case CODEC_MP3:  return "MP3";
    case CODEC_AAC:  return "AAC";
    case CODEC_M4A:  return "M4A";
    case CODEC_FLAC: return "FLAC";
    case CODEC_OGG:  return "OGG";
    case CODEC_WAV:  return "WAV";
    case CODEC_UNKNOWN: break;
  }
  return "Unknown";
}

EspAudioDecoder::~EspAudioDecoder() {
  end();
  freeBuffers();
}

void EspAudioDecoder::registerCodecs() {
  static bool registered = false;
  if (registered) return;

  // Formats enabled in menuconfig (all by default): MP3, AAC, FLAC, Vorbis,
  // Opus, PCM/ADPCM... plus the WAV, M4A, TS and OGG container parsers.
  esp_audio_err_t ret = esp_audio_dec_register_default();
  if (ret != ESP_AUDIO_ERR_OK) ESP_LOGW(TAG, "audio decoder register returned %d", (int)ret);
  ret = esp_audio_simple_dec_register_default();
  if (ret != ESP_AUDIO_ERR_OK) ESP_LOGW(TAG, "simple decoder register returned %d", (int)ret);
  registered = true;
}

bool EspAudioDecoder::allocBuffers() {
  if (inBuf == nullptr) inBuf = (uint8_t*)bufferAlloc(INPUT_BUFFER_SIZE);
  if (outBuf == nullptr) {
    outBuf = (uint8_t*)bufferAlloc(OUTPUT_BUFFER_SIZE);
    outCap = outBuf ? OUTPUT_BUFFER_SIZE : 0;
  }
  if (pcmBuf == nullptr) {
    pcmCap = OUTPUT_BUFFER_SIZE * 2;
    pcmBuf = (int16_t*)bufferAlloc(pcmCap);
    if (pcmBuf == nullptr) pcmCap = 0;
  }
  return inBuf != nullptr && outBuf != nullptr && pcmBuf != nullptr;
}

void EspAudioDecoder::freeBuffers() {
  free(inBuf);
  free(outBuf);
  free(pcmBuf);
  inBuf = nullptr;
  outBuf = nullptr;
  pcmBuf = nullptr;
  outCap = 0;
  pcmCap = 0;
}

bool EspAudioDecoder::begin(CodecHint codec) {
  end();

  esp_audio_simple_dec_type_t type = simpleTypeFor(codec);
  if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
    ESP_LOGE(TAG, "no codec selected");
    return false;
  }
  selectedCodec = codec;

  registerCodecs();
  if (!allocBuffers()) {
    ESP_LOGE(TAG, "buffer allocation failed");
    freeBuffers();
    return false;
  }

  // HE-AAC (AAC+) is common on internet radio; without SBR it plays muffled.
  esp_aac_dec_cfg_t aacCfg = ESP_AAC_DEC_CONFIG_DEFAULT();
  aacCfg.aac_plus_enable = true;

  esp_audio_simple_dec_cfg_t cfg = {};
  cfg.dec_type = type;
  cfg.use_frame_dec = false;
  if (codec == CODEC_AAC) {
    cfg.dec_cfg = &aacCfg;
    cfg.cfg_size = sizeof(aacCfg);
  }

  esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &handle);
  if (ret != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "open %s failed (%d)", codecName(codec), (int)ret);
    handle = nullptr;
    return false;
  }

  inLen = 0;
  headerChecked = false;
  id3Remaining = 0;
  decodeErrors = 0;
  bytesDropped = 0;
  lastSampleRate = 0;
  lastChannels = 0;
  return true;
}

void EspAudioDecoder::end() {
  if (handle != nullptr) {
    esp_audio_simple_dec_close(handle);
    handle = nullptr;
  }
  inLen = 0;
}

void EspAudioDecoder::finish() {
  if (handle == nullptr) return;
  decodeBuffered(true);
  inLen = 0;
}

// Drops a leading ID3v2 tag (cover art can be hundreds of KB and
// contains byte patterns that look like MP3/AAC sync words).
void EspAudioDecoder::skipId3Header() {
  if (!headerChecked) {
    if (inLen < 10) return;
    headerChecked = true;
    bool mayHaveId3 = selectedCodec == CODEC_MP3 || selectedCodec == CODEC_AAC ||
                      selectedCodec == CODEC_FLAC;
    if (mayHaveId3 && memcmp(inBuf, "ID3", 3) == 0) {
      size_t tagSize = ((size_t)(inBuf[6] & 0x7F) << 21) | ((size_t)(inBuf[7] & 0x7F) << 14) |
                       ((size_t)(inBuf[8] & 0x7F) << 7) | (size_t)(inBuf[9] & 0x7F);
      id3Remaining = tagSize + 10 + ((inBuf[5] & 0x10) ? 10 : 0);
      ESP_LOGI(TAG, "skipping ID3v2 tag (%u bytes)", (unsigned)id3Remaining);
    }
  }

  if (id3Remaining > 0 && inLen > 0) {
    size_t drop = std::min(id3Remaining, inLen);
    memmove(inBuf, inBuf + drop, inLen - drop);
    inLen -= drop;
    id3Remaining -= drop;
  }
}

void EspAudioDecoder::write(const uint8_t* data, size_t len) {
  if (handle == nullptr || data == nullptr) return;

  size_t pos = 0;
  while (pos < len) {
    size_t n = std::min(len - pos, INPUT_BUFFER_SIZE - inLen);
    memcpy(inBuf + inLen, data + pos, n);
    inLen += n;
    pos += n;

    skipId3Header();
    if (headerChecked && id3Remaining == 0) {
      decodeBuffered(false);
    }
  }
}

void EspAudioDecoder::decodeBuffered(bool eos) {
  esp_audio_simple_dec_raw_t raw = {};
  raw.buffer = inBuf;
  raw.len = inLen;
  raw.eos = eos;

  int idleRounds = 0;
  while (raw.len > 0 || eos) {
    esp_audio_simple_dec_out_t out = {};
    out.buffer = outBuf;
    out.len = outCap;
    raw.consumed = 0;

    esp_audio_err_t ret = esp_audio_simple_dec_process(handle, &raw, &out);

    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > outCap) {
      uint8_t* grown = (uint8_t*)bufferRealloc(outBuf, out.needed_size);
      if (grown == nullptr) {
        ESP_LOGE(TAG, "output grow to %u failed", (unsigned)out.needed_size);
        bytesDropped += raw.len;
        raw.len = 0;
        break;
      }
      outBuf = grown;
      outCap = out.needed_size;
      continue;
    }

    if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
      esp_audio_simple_dec_info_t decInfo = {};
      if (esp_audio_simple_dec_get_info(handle, &decInfo) == ESP_AUDIO_ERR_OK) {
        emitPcm(outBuf, out.decoded_size, decInfo);
      }
    } else if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_DATA_LACK) {
      decodeErrors++;
      if (decodeErrors <= 5 || (decodeErrors % 100) == 0) {
        ESP_LOGW(TAG, "decode error %d (count %lu)", (int)ret, (unsigned long)decodeErrors);
      }
    }

    uint32_t used = std::min(raw.consumed, raw.len);
    if (used == 0) {
      // No input progress: either the parser needs more bytes, or (at EOS)
      // it is still draining cached frames.
      if (out.decoded_size == 0 || ++idleRounds > 16) break;
      continue;
    }
    idleRounds = 0;
    raw.buffer += used;
    raw.len -= used;
  }

  if (raw.len > 0 && raw.buffer != inBuf) {
    memmove(inBuf, raw.buffer, raw.len);
  }
  inLen = raw.len;

  // A full buffer the parser cannot use is garbage: drop it to resync.
  if (inLen >= INPUT_BUFFER_SIZE) {
    ESP_LOGW(TAG, "dropping %u unparsable bytes", (unsigned)inLen);
    bytesDropped += inLen;
    inLen = 0;
  }
}

void EspAudioDecoder::emitPcm(const uint8_t* pcm, size_t bytes,
                              const esp_audio_simple_dec_info_t& decInfo) {
  size_t bytesPerSample = (decInfo.bits_per_sample + 7) / 8;
  size_t channels = decInfo.channel;
  if (decInfo.sample_rate == 0 || channels == 0 || bytesPerSample == 0 || pcmSink == nullptr) {
    return;
  }

  if (decInfo.sample_rate != lastSampleRate || channels != lastChannels) {
    ESP_LOGI(TAG, "%s %lu Hz, %u ch, %u bit", codecName(selectedCodec),
             (unsigned long)decInfo.sample_rate, (unsigned)channels,
             (unsigned)decInfo.bits_per_sample);
    lastSampleRate = decInfo.sample_rate;
    lastChannels = (uint8_t)channels;
  }

  size_t frameBytes = bytesPerSample * channels;
  size_t frames = bytes / frameBytes;
  size_t needed = frames * 2 * sizeof(int16_t);
  if (needed > pcmCap) {
    int16_t* grown = (int16_t*)bufferRealloc(pcmBuf, needed);
    if (grown == nullptr) {
      bytesDropped += bytes;
      return;
    }
    pcmBuf = grown;
    pcmCap = needed;
  }

  for (size_t i = 0; i < frames; i++) {
    const uint8_t* frame = pcm + i * frameBytes;
    int16_t left = sampleAt(frame, bytesPerSample);
    int16_t right = channels > 1 ? sampleAt(frame + bytesPerSample, bytesPerSample) : left;
    pcmBuf[i * 2] = left;
    pcmBuf[i * 2 + 1] = right;
  }

  pcmSink(pcmBuf, frames, decInfo.sample_rate);
}
