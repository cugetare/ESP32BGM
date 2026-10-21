// ============================================================
//  ESP32BGM — Background Music Module (ESP-IDF)
// ============================================================
#include "app_state.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "main";

// ============================================================
//  CONSTANTS  (declared extern in app_state.h)
// ============================================================

const uint32_t SD_RESCAN_INTERVAL_MS  = 5000;
const size_t   SD_MAX_TRACKS          = 1000;
const size_t   RADIO_BUFFER_SIZE      = 128 * 1024;  // PSRAM
const size_t   RADIO_PREBUFFER_BYTES  = 32 * 1024;
const size_t   AUDIO_CHUNK_SIZE       = 2048;
const int      AUDIO_TASK_STACK_SIZE  = 16 * 1024;
const int      AUDIO_TASK_PRIORITY    = 7;
const int      AUDIO_TASK_CORE        = 1;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
const int      RADIO_MAX_RECOVERIES   = 5;
const uint32_t RECONNECT_INTERVAL_MS  = 5000;
const uint32_t STREAM_TIMEOUT_MS      = 10000;
const char*    SD_MOUNT_POINT         = "/sdcard";
const char*    SD_LIBRARY_ROOT        = "/music";
const char*    CONFIG_AP_SSID         = "ESP32-Radio-Setup";
const char*    CONFIG_AP_PASSWORD     = "radiosetup";

// ============================================================
//  GLOBAL STATE
// ============================================================

std::vector<std::string> sdTracks;
int  currentSDTrack = 0;
bool sdMounted = false;

std::vector<Station> stations;
int currentStation = 0;

RuntimeStatus  runtimeStatus  = { PLAYBACK_RADIO_URL, STATE_STOPPED, NET_OFF, "", "" };
GlobalSettings globalSettings = { PLAYBACK_RADIO_URL, 50, 0, 0 };
WifiSettings   wifiSettings;

EspAudioDecoder audioDecoder;
int audioTaskCore = 0;

uint32_t millisNow() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================
//  APP MAIN
// ============================================================

extern "C" void app_main(void) {
  logStreamInit();  // first, so /logs and TCP clients see the whole boot
  ESP_LOGI(TAG, "ESP32BGM starting, PSRAM free: %u bytes",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  otaBootCheck();
  settingsInit();
  settingsLoadAll();

  if (!audioOutputInit()) {
    ESP_LOGE(TAG, "audio output init failed, continuing without sound");
  }

  networkInit();
  networkBegin();
  bool webOk = startWebServer();
  logStreamStartTcp();

  initAudioTask();
  // Reachable for the next OTA: keep this image (cancels rollback).
  if (webOk) otaMarkHealthy();
  modeBeginSelected();
}
