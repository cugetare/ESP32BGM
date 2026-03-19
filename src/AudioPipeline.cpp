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

  multiDecoder.addDecoder(mp3, "audio/mpeg");
  multiDecoder.addDecoder(aac, "audio/aac");
  multiDecoder.addDecoder(aac, "audio/aacp");
  multiDecoder.addDecoder(aac, "audio/x-aac");
  multiDecoder.addDecoder(aac, "audio/mp4");
  bufferedUrl.setStream(url);
  bufferedUrl.resize(URL_BUFFER_SIZE);
  if (bufferedUrl.availableForWrite() == 0) {
    Serial.printf("URL buffer allocation may have failed at %d bytes, retrying with %d bytes\n", URL_BUFFER_SIZE, URL_BUFFER_FALLBACK_SIZE);
    bufferedUrl.resize(URL_BUFFER_FALLBACK_SIZE);
  }
  multiDecoder.setMimeSource(url);

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
  i2s.setVolume(globalSettings.volume / 100.0f);

  cfg_eq = eq.defaultConfig();
  cfg_eq.setAudioInfo(config);
  applyEQ();

  Serial.printf("Pipeline Reset: SD_Pin_Reserved=%d | Vol=%d\n", config.sd_active, globalSettings.volume);
  Serial.printf("Audio init: sdActive=%d vol=%d bass=%d treble=%d\n",
                sdActive, globalSettings.volume, globalSettings.bass, globalSettings.treble);
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

  bool started = dec.begin();
  if (started) {
    const char* mime = mimeForCodecHint(result.codec);
    if (mime != nullptr) {
      started = multiDecoder.selectDecoder(mime);
    }
  }

  if (!started) {
    runtimeStatus.lastError = "Failed to start decoder";
    runtimeStatus.playbackState = STATE_ERROR;
    activeCodec = CODEC_UNKNOWN;
    return false;
  }

  i2s.setVolume(globalSettings.volume / 100.0f);
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

CodecHint codecHintFromPath(const String& path) {
  String lower = path;
  lower.toLowerCase();
  if (lower.endsWith(".mp3")) return CODEC_MP3;
  if (lower.endsWith(".aac")) return CODEC_AAC;
  return CODEC_UNKNOWN;
}

CodecHint codecHintFromMime(const String& mime) {
  String lower = mime;
  lower.toLowerCase();
  if (lower.indexOf("mpeg") >= 0 || lower.indexOf("mp3") >= 0) return CODEC_MP3;
  if (lower.indexOf("aac") >= 0 || lower.indexOf("mp4") >= 0) return CODEC_AAC;
  return CODEC_UNKNOWN;
}

const char* mimeForCodecHint(CodecHint codec) {
  switch (codec) {
    case CODEC_MP3:
      return "audio/mpeg";
    case CODEC_AAC:
      return "audio/aac";
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
