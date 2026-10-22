// ============================================================
//  AUDIO TASK / DECODED STREAM CONTROL
// ============================================================
#include "app_state.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "audio";

static SemaphoreHandle_t s_stateMutex = nullptr;
static TaskHandle_t s_audioTask = nullptr;

// Volume survives power cycles: saved to NVS once it has been still for
// VOLUME_SAVE_DELAY_US, so a ramp or a run of clicks costs one write.
static const int64_t VOLUME_SAVE_DELAY_US = 5LL * 1000 * 1000;
static esp_timer_handle_t s_volumeSaveTimer = nullptr;
static int s_savedVolume = -1;

static void volumeSaveCallback(void* arg) {
  (void)arg;
  int volume = globalSettings.volume;
  if (volume == s_savedVolume) return;
  settingsSaveVolume();
  s_savedVolume = volume;
  ESP_LOGI(TAG, "volume %d saved", volume);
}

static void scheduleVolumeSave() {
  if (s_volumeSaveTimer == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = volumeSaveCallback;
    args.name = "volSave";
    if (esp_timer_create(&args, &s_volumeSaveTimer) != ESP_OK) return;
  }
  esp_timer_stop(s_volumeSaveTimer);  // restart the quiet period
  esp_timer_start_once(s_volumeSaveTimer, VOLUME_SAVE_DELAY_US);
}

static std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return value;
}

static bool endsWith(const std::string& value, const char* suffix) {
  size_t n = strlen(suffix);
  return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
}

static void decoderSink(const int16_t* stereo, size_t frames, uint32_t sampleRate) {
  audioOutputWrite(stereo, frames, sampleRate);
}

// The audio task runs one source iteration (one chunk read + decode)
// per lock hold, so web handlers can interleave state changes between
// chunks without any hold/idle handshake.
static void audioTaskRunner(void* parameter) {
  (void)parameter;
  for (;;) {
    bool worked = false;
    if (audioStateLock(100)) {
      worked = modeLoop();
      audioStateUnlock();
    }
    // Always yield one tick so lower-priority waiters can take the lock.
    vTaskDelay(worked ? 1 : pdMS_TO_TICKS(10));
  }
}

void initAudioTask() {
  if (s_audioTask != nullptr) return;
  if (s_stateMutex == nullptr) s_stateMutex = xSemaphoreCreateRecursiveMutex();

  audioDecoder.setSink(decoderSink);
  s_savedVolume = globalSettings.volume;  // loaded from NVS at boot
  audioTaskCore = AUDIO_TASK_CORE;
  xTaskCreatePinnedToCore(audioTaskRunner, "audioTask", AUDIO_TASK_STACK_SIZE, nullptr,
                          AUDIO_TASK_PRIORITY, &s_audioTask, AUDIO_TASK_CORE);
}

bool audioStateLock(uint32_t timeoutMs) {
  if (s_stateMutex == nullptr) s_stateMutex = xSemaphoreCreateRecursiveMutex();
  return xSemaphoreTakeRecursive(s_stateMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void audioStateUnlock() {
  if (s_stateMutex != nullptr) xSemaphoreGiveRecursive(s_stateMutex);
}

bool startDecodedStream(const SourceOpenResult& result) {
  if (!audioDecoder.begin(result.codec)) {
    runtimeStatus.lastError = std::string("Failed to start ") +
                              EspAudioDecoder::codecName(result.codec) + " decoder";
    runtimeStatus.playbackState = STATE_ERROR;
    return false;
  }

  ESP_LOGI(TAG, "playing %s (%s)", result.title.c_str(), EspAudioDecoder::codecName(result.codec));
  runtimeStatus.nowPlaying = result.title.empty() ? "Unknown" : result.title;
  runtimeStatus.lastError.clear();
  runtimeStatus.playbackState = result.isLive ? STATE_BUFFERING : STATE_PLAYING;
  return true;
}

void stopDecodedStream() {
  if (audioDecoder.active()) {
    audioDecoder.end();
    audioOutputSilence();
  }
  runtimeStatus.playbackState = STATE_STOPPED;
}

void playbackSetVolume(int value) {
  globalSettings.volume = std::clamp(value, 0, 100);
  audioOutputSetVolume(globalSettings.volume);
  dspApplySettings();  // the enhancer's depth follows the volume
  scheduleVolumeSave();
}

void playbackSetBass(int value) {
  globalSettings.bass = std::clamp(value, -10, 10);
  dspApplySettings();
}

void playbackSetTreble(int value) {
  globalSettings.treble = std::clamp(value, -10, 10);
  dspApplySettings();
}

// key: mono | enh | hpf | nhz | ndb
bool playbackSetSound(const std::string& key, int value) {
  if (key == "mono") soundSettings.mono = value != 0;
  else if (key == "enh") soundSettings.enhancer = std::clamp(value, 0, 100);
  else if (key == "hpf") soundSettings.protectHz = value <= 0 ? 0 : std::clamp(value, 40, 400);
  else if (key == "nhz") soundSettings.notchHz = std::clamp(value, 100, 2000);
  else if (key == "ndb") soundSettings.notchDepthDb = std::clamp(value, 0, 12);
  else return false;
  dspApplySettings();
  return true;
}

bool mimeIsPlaylist(const std::string& mime) {
  std::string lower = toLower(mime);
  return lower.find("mpegurl") != std::string::npos || lower.find("scpls") != std::string::npos ||
         lower.find("x-pls") != std::string::npos;
}

CodecHint codecHintFromMime(const std::string& mime) {
  std::string lower = toLower(mime);
  if (mimeIsPlaylist(lower)) return CODEC_UNKNOWN;
  if (lower.find("mpeg") != std::string::npos || lower.find("mp3") != std::string::npos) return CODEC_MP3;
  if (lower.find("aac") != std::string::npos) return CODEC_AAC;
  if (lower.find("mp4") != std::string::npos || lower.find("m4a") != std::string::npos) return CODEC_M4A;
  if (lower.find("flac") != std::string::npos) return CODEC_FLAC;
  if (lower.find("ogg") != std::string::npos || lower.find("opus") != std::string::npos ||
      lower.find("vorbis") != std::string::npos) return CODEC_OGG;
  if (lower.find("wav") != std::string::npos) return CODEC_WAV;
  return CODEC_UNKNOWN;
}

CodecHint codecHintFromPath(const std::string& path) {
  std::string lower = toLower(path);
  if (endsWith(lower, ".mp3")) return CODEC_MP3;
  if (endsWith(lower, ".aac")) return CODEC_AAC;
  if (endsWith(lower, ".m4a") || endsWith(lower, ".mp4")) return CODEC_M4A;
  if (endsWith(lower, ".flac")) return CODEC_FLAC;
  if (endsWith(lower, ".ogg") || endsWith(lower, ".oga") || endsWith(lower, ".opus")) return CODEC_OGG;
  if (endsWith(lower, ".wav")) return CODEC_WAV;
  return CODEC_UNKNOWN;
}
