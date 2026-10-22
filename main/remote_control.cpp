// ============================================================
//  REMOTE CONTROL
//  One command set for MQTT (Home Assistant) and HTTP (/cmd, /set):
//  volume steps and ramps run right away so a held button feels
//  direct; anything that switches sources or streams goes to a
//  worker task so it never blocks the MQTT client (and with it the
//  "ramp_stop" that ends a hold).
// ============================================================
#include "app_state.h"

#include <string.h>
#include <algorithm>

#include "esp_log.h"
#include "freertos/queue.h"

static const char* TAG = "remote";

static const int      VOLUME_STEP       = 5;     // one click
static const uint32_t RAMP_INTERVAL_MS  = 60;    // one volume point per tick while held
static const uint32_t RAMP_MAX_MS       = 8000;  // safety stop if the release never arrives

struct RemoteJob {
  char key[16];
  char value[112];
};

static QueueHandle_t s_jobs = nullptr;
static TaskHandle_t s_rampTask = nullptr;
static volatile int s_rampDir = 0;
static volatile uint32_t s_rampStart = 0;

// ============================================================
//  VOLUME RAMP
// ============================================================
static void rampTask(void* arg) {
  (void)arg;
  for (;;) {
    if (s_rampDir == 0) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    if (millisNow() - s_rampStart > RAMP_MAX_MS) {
      ESP_LOGW(TAG, "volume ramp timed out (no release received)");
      s_rampDir = 0;
      continue;
    }
    int next = globalSettings.volume + s_rampDir;
    if (next < 0 || next > 100) {
      s_rampDir = 0;  // reached the end; stay there
      continue;
    }
    playbackSetVolume(next);
    vTaskDelay(pdMS_TO_TICKS(RAMP_INTERVAL_MS));
  }
}

static void rampStart(int dir) {
  s_rampStart = millisNow();
  s_rampDir = dir;
  if (s_rampTask) xTaskNotifyGive(s_rampTask);
}

bool remoteRamping() {
  return s_rampDir != 0;
}

// ============================================================
//  SLOW ACTIONS (worker task)
// ============================================================
static bool stepItem(int dir) {
  if (!audioStateLock(2000)) return false;
  int count = 0, current = 0;
  if (runtimeStatus.selectedMode == PLAYBACK_RADIO_URL) {
    count = (int)stations.size();
    current = currentStation;
  } else if (runtimeStatus.selectedMode == PLAYBACK_SD_CARD) {
    count = (int)sdTracks.size();
    current = currentSDTrack;
  }
  bool ok = false;
  if (count > 0) ok = playbackSelectItem(((current + dir) % count + count) % count);
  audioStateUnlock();
  return ok;
}

static bool stepSource(int dir) {
  // FTP is not implemented, so cycling only covers radio and SD.
  (void)dir;
  PlaybackMode next = runtimeStatus.selectedMode == PLAYBACK_RADIO_URL ? PLAYBACK_SD_CARD
                                                                       : PLAYBACK_RADIO_URL;
  return modeSwitchTo(next);
}

static bool selectStationByName(const std::string& name) {
  if (!audioStateLock(2000)) return false;
  int index = -1;
  for (size_t i = 0; i < stations.size(); ++i) {
    if (stations[i].name == name) { index = (int)i; break; }
  }
  bool needSwitch = runtimeStatus.selectedMode != PLAYBACK_RADIO_URL;
  audioStateUnlock();
  if (index < 0) return false;

  // Picking a station implies radio.
  if (needSwitch) {
    currentStation = index;
    settingsSaveRadio();
    return modeSwitchTo(PLAYBACK_RADIO_URL);
  }
  return playbackSelectItem(index);
}

static void runJob(const RemoteJob& job) {
  std::string key = job.key, value = job.value;
  bool ok = false;
  if (key == "cmd") {
    if (value == "next") ok = stepItem(+1);
    else if (value == "previous") ok = stepItem(-1);
    else if (value == "source_next") ok = stepSource(+1);
    else if (value == "source_prev") ok = stepSource(-1);
    else if (value == "play") ok = playbackPlay();
    else if (value == "pause") { playbackPause(); ok = true; }
    else if (value == "toggle") {
      bool playing = runtimeStatus.playbackState == STATE_PLAYING ||
                     runtimeStatus.playbackState == STATE_BUFFERING;
      if (playing) { playbackPause(); ok = true; } else ok = playbackPlay();
    }
  } else if (key == "source") {
    if (value == "Radio") ok = modeSwitchTo(PLAYBACK_RADIO_URL);
    else if (value == "SD Card") ok = modeSwitchTo(PLAYBACK_SD_CARD);
  } else if (key == "station") {
    ok = selectStationByName(value);
  } else if (key == "play") {
    if (value == "ON") ok = playbackPlay();
    else { playbackPause(); ok = true; }
  }
  if (!ok) ESP_LOGW(TAG, "%s=%s failed: %s", key.c_str(), value.c_str(), runtimeStatus.lastError.c_str());
}

static void workerTask(void* arg) {
  (void)arg;
  RemoteJob job;
  for (;;) {
    if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) runJob(job);
  }
}

static bool enqueue(const char* key, const std::string& value) {
  if (s_jobs == nullptr) return false;
  RemoteJob job = {};
  strlcpy(job.key, key, sizeof(job.key));
  strlcpy(job.value, value.c_str(), sizeof(job.value));
  if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
    ESP_LOGW(TAG, "command queue full, dropped %s=%s", key, job.value);
    return false;
  }
  return true;
}

// ============================================================
//  PUBLIC API
// ============================================================
void remoteInit() {
  if (s_jobs != nullptr) return;
  s_jobs = xQueueCreate(8, sizeof(RemoteJob));
  xTaskCreatePinnedToCore(rampTask, "volRamp", 3072, nullptr, 6, &s_rampTask, 0);
  xTaskCreatePinnedToCore(workerTask, "remoteCmd", 6144, nullptr, 4, nullptr, 0);
}

// cmd: volume_up | volume_down | ramp_up | ramp_down | ramp_stop |
//      next | previous | source_next | source_prev | play | pause | toggle
bool remoteCommand(const std::string& cmd) {
  if (cmd == "volume_up" || cmd == "volume_down") {
    s_rampDir = 0;
    int dir = cmd == "volume_up" ? 1 : -1;
    playbackSetVolume(std::clamp(globalSettings.volume + dir * VOLUME_STEP, 0, 100));
    return true;
  }
  if (cmd == "ramp_up") { rampStart(+1); return true; }
  if (cmd == "ramp_down") { rampStart(-1); return true; }
  if (cmd == "ramp_stop") { s_rampDir = 0; return true; }
  if (cmd == "next" || cmd == "previous" || cmd == "source_next" || cmd == "source_prev" ||
      cmd == "play" || cmd == "pause" || cmd == "toggle") {
    return enqueue("cmd", cmd);
  }
  return false;
}

// key: volume | mono | enh | hpf | nhz | ndb (immediate)
//      source | station | play (queued)
bool remoteSet(const std::string& key, const std::string& value) {
  if (key == "volume") {
    s_rampDir = 0;
    playbackSetVolume(atoi(value.c_str()));
    return true;
  }
  if (key == "mono") return playbackSetSound("mono", value == "ON" || value == "1");
  if (key == "enh" || key == "hpf" || key == "nhz" || key == "ndb") {
    return playbackSetSound(key, atoi(value.c_str()));
  }
  if (key == "source" || key == "station" || key == "play") return enqueue(key.c_str(), value);
  return false;
}
