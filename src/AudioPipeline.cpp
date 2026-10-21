// ============================================================
//  HARDWARE / AUDIO PIPELINE
// ============================================================
#include "globals.h"

void initHardware() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);

  if (psramFound()) {
    Serial.printf("PSRAM: %lu bytes\n", (unsigned long)ESP.getPsramSize());
  } else {
    Serial.println("No PSRAM");
  }

  // Equalizer3Bands does not forward format changes, so the I2S/codec must
  // hear about sample-rate changes straight from the decoder.
  audioDecoder.addNotifyAudioChange(i2s);
  bufferedUrl.setStream(url);
  bufferedUrl.resize(URL_BUFFER_SIZE);
  if (bufferedUrl.availableForWrite() == 0) {
    Serial.printf("URL buffer allocation failed at %d bytes, retrying with %d bytes\n", URL_BUFFER_SIZE, URL_BUFFER_FALLBACK_SIZE);
    bufferedUrl.resize(URL_BUFFER_FALLBACK_SIZE);
  }
  Serial.printf("URL buffer size: %d bytes (available: %d)\n",
                 (int)bufferedUrl.availableForWrite(),
                 (int)URL_BUFFER_SIZE - (int)bufferedUrl.availableForWrite());

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS failed, continuing without storage");
  } else {
    Serial.println("LittleFS OK");
  }
}

// ============================================================
//  INIT AUDIO PIPELINE
// ============================================================
void initAudioPipeline(bool sdActive) {
  dec.end();
  activeCodec = CODEC_UNKNOWN;

  auto config = i2s.defaultConfig(TX_MODE);
  config.sd_active = sdActive;
  if (!sdActive) {
    config.buffer_size = 1024;
    config.buffer_count = 16;
  }

  if (!i2s.begin(config)) {
    Serial.println("ERROR: Failed to initialize I2S");
    return;
  }

  int clampedVolume = constrain(globalSettings.volume, 0, 100);
  i2s.setVolume(clampedVolume / 100.0f);

  cfg_eq = eq.defaultConfig();
  cfg_eq.setAudioInfo(config);
  applyEQ();

  Serial.printf("Pipeline Reset: SD_Pin_Reserved=%d | Vol=%d\n", config.sd_active, clampedVolume);
  Serial.printf("Audio init: sdActive=%d vol=%d bass=%d treble=%d\n",
                sdActive, clampedVolume, globalSettings.bass, globalSettings.treble);
}

// ============================================================
//  APPLY EQ
// ============================================================
void applyEQ() {
  cfg_eq.gain_low = 1.0f + (globalSettings.bass / 10.0f);
  cfg_eq.gain_medium = 1.0f;
  cfg_eq.gain_high = 1.0f + (globalSettings.treble / 10.0f);
  eq.begin(cfg_eq);
}

bool startDecodedStream(const SourceOpenResult& result) {
  if (result.stream == nullptr) {
    runtimeStatus.lastError = "No input stream available";
    runtimeStatus.playbackState = STATE_ERROR;
    return false;
  }

  dec.end();
  activeCodec = result.codec;

  bool started = audioDecoder.setCodec(result.codec) && dec.begin();

  if (!started) {
    runtimeStatus.lastError = "Failed to start decoder";
    runtimeStatus.playbackState = STATE_ERROR;
    activeCodec = CODEC_UNKNOWN;
    return false;
  }

  int clampedVolume = constrain(globalSettings.volume, 0, 100);
  i2s.setVolume(clampedVolume / 100.0f);
  runtimeStatus.nowPlaying = (result.title != nullptr) ? result.title : "Unknown";
  runtimeStatus.lastError = "";
  runtimeStatus.playbackState = STATE_PLAYING;
  lastAudioData = millis();
  return true;
}

void stopDecodedStream() {
  dec.end();
  activeCodec = CODEC_UNKNOWN;
  runtimeStatus.playbackState = STATE_STOPPED;
}

void playbackSetVolume(int value) {
  if (!audioStateLock(250)) return;
  globalSettings.volume = constrain(value, 0, 100);
  i2s.setVolume(globalSettings.volume / 100.0f);
  audioStateUnlock();
}

void playbackSetBass(int value) {
  if (!audioStateLock(250)) return;
  globalSettings.bass = constrain(value, -10, 10);
  applyEQ();
  audioStateUnlock();
}

void playbackSetTreble(int value) {
  if (!audioStateLock(250)) return;
  globalSettings.treble = constrain(value, -10, 10);
  applyEQ();
  audioStateUnlock();
}

CodecHint codecHintFromMime(const String& mime) {
  String lower = mime;
  lower.toLowerCase();
  if (lower.indexOf("mpegurl") >= 0 || lower.indexOf("scpls") >= 0) return CODEC_UNKNOWN;  // playlists
  if (lower.indexOf("mpeg") >= 0 || lower.indexOf("mp3") >= 0) return CODEC_MP3;
  if (lower.indexOf("aac") >= 0) return CODEC_AAC;
  if (lower.indexOf("mp4") >= 0 || lower.indexOf("m4a") >= 0) return CODEC_M4A;
  if (lower.indexOf("flac") >= 0) return CODEC_FLAC;
  if (lower.indexOf("ogg") >= 0 || lower.indexOf("opus") >= 0 || lower.indexOf("vorbis") >= 0) return CODEC_OGG;
  if (lower.indexOf("wav") >= 0) return CODEC_WAV;
  return CODEC_UNKNOWN;
}

const char* mimeForCodecHint(CodecHint codec) {
  switch (codec) {
    case CODEC_MP3:  return "audio/mpeg";
    case CODEC_AAC:  return "audio/aac";
    case CODEC_M4A:  return "audio/mp4";
    case CODEC_FLAC: return "audio/flac";
    case CODEC_OGG:  return "audio/ogg";
    case CODEC_WAV:  return "audio/wav";
    case CODEC_UNKNOWN:
      break;
  }
  return nullptr;
}

float readChipTemperatureC() {
  return temperatureRead();
}

String formatChipTemperature() {
  float chipTempC = readChipTemperatureC();
  if (isnan(chipTempC)) {
    return "Unavailable";
  }
  return String(chipTempC, 1) + " C";
}

CodecHint codecHintFromPath(const String& path) {
  String lower = path;
  lower.toLowerCase();
  if (lower.endsWith(".mp3")) return CODEC_MP3;
  if (lower.endsWith(".aac")) return CODEC_AAC;
  if (lower.endsWith(".m4a") || lower.endsWith(".mp4")) return CODEC_M4A;
  if (lower.endsWith(".flac")) return CODEC_FLAC;
  if (lower.endsWith(".ogg") || lower.endsWith(".oga") || lower.endsWith(".opus")) return CODEC_OGG;
  if (lower.endsWith(".wav")) return CODEC_WAV;
  return CODEC_UNKNOWN;
}
