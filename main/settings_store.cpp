// ============================================================
//  SETTINGS STORE (NVS namespace "radio", same keys as the
//  Arduino Preferences version; stations as a JSON blob)
// ============================================================
#include "app_state.h"

#include <algorithm>
#include <memory>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "settings";
static const char* NVS_NAMESPACE = "radio";
static const char* STATIONS_KEY = "stations";

void settingsInit() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition reset (%s)", esp_err_to_name(ret));
    nvs_flash_erase();
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

static std::string readString(nvs_handle_t h, const char* key, const char* fallback = "") {
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return fallback;
  std::string value(len, '\0');
  if (nvs_get_str(h, key, &value[0], &len) != ESP_OK) return fallback;
  value.resize(len - 1);  // drop terminator
  return value;
}

static int readInt(nvs_handle_t h, const char* key, int fallback) {
  int32_t value = fallback;
  nvs_get_i32(h, key, &value);
  return (int)value;
}

static void writeInts(std::initializer_list<std::pair<const char*, int>> values) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  for (const auto& kv : values) nvs_set_i32(h, kv.first, kv.second);
  nvs_commit(h);
  nvs_close(h);
}

void settingsSaveWifi(const std::string& ssid, const std::string& password) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, "wifi_ssid", ssid.c_str());
    nvs_set_str(h, "wifi_pass", password.c_str());
    nvs_commit(h);
    nvs_close(h);
  }
  wifiSettings.ssid = ssid;
  wifiSettings.password = password;
}

void settingsClearWifi() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "wifi_ssid");
    nvs_erase_key(h, "wifi_pass");
    nvs_commit(h);
    nvs_close(h);
  }
  wifiSettings.ssid.clear();
  wifiSettings.password.clear();
}

void settingsLoadAll() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    int mode = readInt(h, "mode", PLAYBACK_RADIO_URL);
    if (mode < PLAYBACK_RADIO_URL || mode > PLAYBACK_FTP_LIBRARY) mode = PLAYBACK_RADIO_URL;
    globalSettings.selectedMode = (PlaybackMode)mode;
    globalSettings.volume = std::clamp(readInt(h, "volume", 50), 0, 100);
    globalSettings.bass = std::clamp(readInt(h, "bass", 0), -10, 10);
    globalSettings.treble = std::clamp(readInt(h, "treble", 0), -10, 10);
    soundSettings.mono = readInt(h, "mono", 0) != 0;
    soundSettings.enhancer = std::clamp(readInt(h, "enh", 0), 0, 100);
    soundSettings.protectHz = std::clamp(readInt(h, "hpf", 80), 0, 400);
    soundSettings.notchHz = std::clamp(readInt(h, "notch_hz", 400), 100, 2000);
    soundSettings.notchDepthDb = std::clamp(readInt(h, "notch_db", 0), 0, 12);
    mqttSettings.enabled = readInt(h, "mqtt_en", 0) != 0;
    mqttSettings.host = readString(h, "mqtt_host");
    mqttSettings.port = std::clamp(readInt(h, "mqtt_port", 1883), 1, 65535);
    mqttSettings.user = readString(h, "mqtt_user");
    mqttSettings.password = readString(h, "mqtt_pass");
    mqttSettings.name = readString(h, "mqtt_name");
    currentStation = readInt(h, "station", 0);
    currentSDTrack = readInt(h, "sd_track", 0);
    wifiSettings.ssid = readString(h, "wifi_ssid");
    wifiSettings.password = readString(h, "wifi_pass");
    nvs_close(h);
  }
  runtimeStatus.selectedMode = globalSettings.selectedMode;
}

void settingsSaveGlobal() {
  writeInts({ { "mode", (int)globalSettings.selectedMode },
              { "volume", globalSettings.volume },
              { "bass", globalSettings.bass },
              { "treble", globalSettings.treble },
              { "mono", soundSettings.mono ? 1 : 0 },
              { "enh", soundSettings.enhancer },
              { "hpf", soundSettings.protectHz },
              { "notch_hz", soundSettings.notchHz },
              { "notch_db", soundSettings.notchDepthDb } });
}

void settingsSaveMqtt() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_i32(h, "mqtt_en", mqttSettings.enabled ? 1 : 0);
  nvs_set_i32(h, "mqtt_port", mqttSettings.port);
  nvs_set_str(h, "mqtt_host", mqttSettings.host.c_str());
  nvs_set_str(h, "mqtt_user", mqttSettings.user.c_str());
  nvs_set_str(h, "mqtt_pass", mqttSettings.password.c_str());
  nvs_set_str(h, "mqtt_name", mqttSettings.name.c_str());
  nvs_commit(h);
  nvs_close(h);
}

void settingsSaveRadio() {
  writeInts({ { "station", currentStation } });
}

void settingsSaveSd() {
  writeInts({ { "sd_track", currentSDTrack } });
}

// ============================================================
//  STATION STORAGE
// ============================================================
static void loadDefaultStations() {
  stations = {
    { "Radio Paradise - Mellow", "http://stream.radioparadise.com/mellow-192", "audio/mpeg" },
    { "Radio Paradise - Main", "http://stream.radioparadise.com/aac-320", "audio/aac" },
    { "Radio Paradise - Rock", "http://stream.radioparadise.com/rock-320", "audio/aac" },
    { "SomaFM - Groove Salad", "http://ice1.somafm.com/groovesalad-256-mp3", "audio/mpeg" },
    { "SomaFM - Drone Zone", "http://ice1.somafm.com/dronezone-256-mp3", "audio/mpeg" },
    { "SomaFM - Lush", "http://ice1.somafm.com/lush-128-mp3", "audio/mpeg" },
  };
}

void loadStations() {
  stations.clear();

  std::string json;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    size_t len = 0;
    if (nvs_get_blob(h, STATIONS_KEY, nullptr, &len) == ESP_OK && len > 0) {
      json.resize(len);
      if (nvs_get_blob(h, STATIONS_KEY, &json[0], &len) != ESP_OK) json.clear();
    }
    nvs_close(h);
  }

  if (json.empty()) {
    ESP_LOGI(TAG, "no stations stored, creating defaults");
    loadDefaultStations();
    saveStations();
    return;
  }

  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> doc(cJSON_ParseWithLength(json.data(), json.size()),
                                                     cJSON_Delete);
  const cJSON* arr = doc ? cJSON_GetObjectItem(doc.get(), "stations") : nullptr;
  if (!cJSON_IsArray(arr)) {
    ESP_LOGW(TAG, "failed to parse stored stations, using defaults");
    loadDefaultStations();
    return;
  }

  const cJSON* obj;
  cJSON_ArrayForEach(obj, arr) {
    const cJSON* name = cJSON_GetObjectItem(obj, "name");
    const cJSON* url = cJSON_GetObjectItem(obj, "url");
    const cJSON* mime = cJSON_GetObjectItem(obj, "mime");
    if (!cJSON_IsString(name) || !cJSON_IsString(url)) continue;
    stations.push_back({ name->valuestring, url->valuestring,
                         cJSON_IsString(mime) ? mime->valuestring : "" });
  }
  ESP_LOGI(TAG, "stations loaded: %u", (unsigned)stations.size());
}

void saveStations() {
  cJSON* doc = cJSON_CreateObject();
  cJSON* arr = cJSON_AddArrayToObject(doc, "stations");
  for (const auto& s : stations) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", s.name.c_str());
    cJSON_AddStringToObject(obj, "url", s.url.c_str());
    cJSON_AddStringToObject(obj, "mime", s.mime.c_str());
    cJSON_AddItemToArray(arr, obj);
  }
  char* json = cJSON_PrintUnformatted(doc);
  cJSON_Delete(doc);
  if (json == nullptr) return;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    if (nvs_set_blob(h, STATIONS_KEY, json, strlen(json)) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
      ESP_LOGE(TAG, "failed to save stations");
    }
    nvs_close(h);
  }
  cJSON_free(json);
  ESP_LOGI(TAG, "stations saved: %u", (unsigned)stations.size());
}
