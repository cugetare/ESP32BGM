// ============================================================
//  MQTT BRIDGE (Home Assistant)
//  - publishes MQTT discovery so the board shows up in HA as one
//    device with volume, source, station, play, next/previous,
//    now-playing and the sound settings
//  - one retained JSON state topic, republished on change
//  - commands:  <base>/set/<key>  (volume, source, station, play,
//               mono, enh, hpf, nhz, ndb)
//               <base>/cmd        (volume_up/down, ramp_up/down/stop,
//               next, previous, source_next/prev, play, pause, toggle)
//  - availability: <base>/status = online / offline (LWT)
// ============================================================
#include "app_state.h"

#include <string.h>
#include <memory>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

static const char* TAG = "mqtt";
static const char* DISCOVERY_PREFIX = "homeassistant";
static const uint32_t STATE_INTERVAL_MS = 250;
static const uint32_t RSSI_INTERVAL_MS = 60000;

static esp_mqtt_client_handle_t s_client = nullptr;
static SemaphoreHandle_t s_clientLock = nullptr;  // state task vs. stop/restart
static TaskHandle_t s_stateTask = nullptr;
static volatile bool s_connected = false;
static volatile bool s_needDiscovery = false;
static std::string s_deviceId;   // bgm_xxxxxx
static std::string s_base;       // esp32bgm/xxxxxx
static std::string s_lastState;
static std::string s_lastStations;
static std::string s_uri;
static int s_rssi = 0;
static uint32_t s_lastRssi = 0;

// ============================================================
//  IDENTITY
// ============================================================
static void initIdentity() {
  if (!s_deviceId.empty()) return;
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  s_deviceId = std::string("bgm_") + suffix;
  s_base = std::string("esp32bgm/") + suffix;
}

std::string mqttDeviceSuffix() {
  initIdentity();
  return s_deviceId.substr(4);
}

const char* mqttStatusText() {
  if (!mqttSettings.enabled) return "Disabled";
  if (mqttSettings.host.empty()) return "No broker configured";
  return s_connected ? "Connected" : "Not connected";
}

std::string mqttBaseTopic() {
  initIdentity();
  return s_base;
}

// ============================================================
//  DISCOVERY
// ============================================================
using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

static void publishJson(const std::string& topic, cJSON* obj, bool retain) {
  char* text = cJSON_PrintUnformatted(obj);
  if (text == nullptr) return;
  esp_mqtt_client_enqueue(s_client, topic.c_str(), text, 0, 1, retain ? 1 : 0, true);
  cJSON_free(text);
}

static cJSON* deviceBlock() {
  cJSON* dev = cJSON_CreateObject();
  cJSON* ids = cJSON_AddArrayToObject(dev, "identifiers");
  cJSON_AddItemToArray(ids, cJSON_CreateString(s_deviceId.c_str()));
  std::string name = mqttSettings.name.empty() ? "ESP32BGM " + s_deviceId.substr(4) : mqttSettings.name;
  cJSON_AddStringToObject(dev, "name", name.c_str());
  cJSON_AddStringToObject(dev, "manufacturer", "ESP32BGM");
  cJSON_AddStringToObject(dev, "model", "Ai-Thinker ESP32 Audio Kit (ES8388)");
  cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
  std::string ip = networkStaIp();
  if (!ip.empty()) cJSON_AddStringToObject(dev, "configuration_url", ("http://" + ip + "/").c_str());
  return dev;
}

// Common fields; caller adds the component-specific ones.
static cJSON* entity(const char* key, const char* name, const char* icon, const char* category) {
  cJSON* c = cJSON_CreateObject();
  cJSON_AddStringToObject(c, "name", name);
  cJSON_AddStringToObject(c, "unique_id", (s_deviceId + "_" + key).c_str());
  cJSON_AddStringToObject(c, "availability_topic", (s_base + "/status").c_str());
  if (icon) cJSON_AddStringToObject(c, "icon", icon);
  if (category) cJSON_AddStringToObject(c, "entity_category", category);
  cJSON_AddItemToObject(c, "device", deviceBlock());
  return c;
}

static void addState(cJSON* c, const char* field) {
  cJSON_AddStringToObject(c, "state_topic", (s_base + "/state").c_str());
  cJSON_AddStringToObject(c, "value_template", (std::string("{{ value_json.") + field + " }}").c_str());
}

static void announce(const char* component, const char* key, cJSON* c) {
  std::string topic = std::string(DISCOVERY_PREFIX) + "/" + component + "/" + s_deviceId + "/" + key + "/config";
  publishJson(topic, c, true);
  cJSON_Delete(c);
}

static void announceNumber(const char* key, const char* name, const char* icon, const char* category,
                           int min, int max, int step, const char* unit, const char* mode) {
  cJSON* c = entity(key, name, icon, category);
  addState(c, key);
  cJSON_AddStringToObject(c, "command_topic", (s_base + "/set/" + key).c_str());
  cJSON_AddNumberToObject(c, "min", min);
  cJSON_AddNumberToObject(c, "max", max);
  cJSON_AddNumberToObject(c, "step", step);
  if (unit) cJSON_AddStringToObject(c, "unit_of_measurement", unit);
  cJSON_AddStringToObject(c, "mode", mode);
  announce("number", key, c);
}

static void announceButton(const char* key, const char* name, const char* icon, const char* payload) {
  cJSON* c = entity(key, name, icon, nullptr);
  cJSON_AddStringToObject(c, "command_topic", (s_base + "/cmd").c_str());
  cJSON_AddStringToObject(c, "payload_press", payload);
  announce("button", key, c);
}

static void announceSensor(const char* key, const char* name, const char* icon, const char* category,
                           const char* unit, const char* deviceClass) {
  cJSON* c = entity(key, name, icon, category);
  addState(c, key);
  if (unit) cJSON_AddStringToObject(c, "unit_of_measurement", unit);
  if (deviceClass) cJSON_AddStringToObject(c, "device_class", deviceClass);
  announce("sensor", key, c);
}

static void announceSwitch(const char* key, const char* name, const char* icon, const char* category) {
  cJSON* c = entity(key, name, icon, category);
  addState(c, key);
  cJSON_AddStringToObject(c, "command_topic", (s_base + "/set/" + key).c_str());
  announce("switch", key, c);
}

// Station names, under the state lock.
static std::vector<std::string> stationNames() {
  std::vector<std::string> names;
  if (audioStateLock(500)) {
    for (const auto& s : stations) names.push_back(s.name);
    audioStateUnlock();
  }
  return names;
}

static void announceStationSelect(const std::vector<std::string>& names) {
  cJSON* c = entity("station", "Station", "mdi:radio", nullptr);
  addState(c, "station");
  cJSON_AddStringToObject(c, "command_topic", (s_base + "/set/station").c_str());
  cJSON* opts = cJSON_AddArrayToObject(c, "options");
  for (const auto& n : names) cJSON_AddItemToArray(opts, cJSON_CreateString(n.c_str()));
  if (names.empty()) cJSON_AddItemToArray(opts, cJSON_CreateString("-"));
  announce("select", "station", c);
}

static void publishDiscovery() {
  announceNumber("volume", "Volume", "mdi:volume-high", nullptr, 0, 100, 1, "%", "slider");

  cJSON* src = entity("source", "Source", "mdi:music-box-multiple", nullptr);
  addState(src, "source");
  cJSON_AddStringToObject(src, "command_topic", (s_base + "/set/source").c_str());
  cJSON* opts = cJSON_AddArrayToObject(src, "options");
  cJSON_AddItemToArray(opts, cJSON_CreateString("Radio"));
  cJSON_AddItemToArray(opts, cJSON_CreateString("SD Card"));
  announce("select", "source", src);

  std::vector<std::string> names = stationNames();
  announceStationSelect(names);
  s_lastStations.clear();
  for (const auto& n : names) s_lastStations += n + "\n";

  announceSwitch("play", "Play", "mdi:play-pause", nullptr);
  announceButton("next", "Next", "mdi:skip-next", "next");
  announceButton("previous", "Previous", "mdi:skip-previous", "previous");
  announceButton("volume_up", "Volume up", "mdi:volume-plus", "volume_up");
  announceButton("volume_down", "Volume down", "mdi:volume-minus", "volume_down");
  announceSensor("now_playing", "Now playing", "mdi:music", nullptr, nullptr, nullptr);
  announceSensor("status", "Status", "mdi:information-outline", nullptr, nullptr, nullptr);
  announceSensor("stream", "Stream", "mdi:waveform", "diagnostic", nullptr, nullptr);

  announceSwitch("mono", "Mono", "mdi:speaker", "config");
  announceNumber("enh", "Sound enhancer", "mdi:auto-fix", "config", 0, 100, 1, nullptr, "slider");
  announceNumber("hpf", "Exciter protection", "mdi:sine-wave", "config", 0, 400, 10, "Hz", "slider");
  announceNumber("nhz", "Resonance notch", "mdi:tune-vertical", "config", 100, 2000, 10, "Hz", "slider");
  announceNumber("ndb", "Notch depth", "mdi:arrow-collapse-down", "config", 0, 12, 1, "dB", "slider");

  announceSensor("rssi", "Wi-Fi signal", "mdi:wifi", "diagnostic", "dBm", "signal_strength");
  announceSensor("ip", "IP address", "mdi:ip-network", "diagnostic", nullptr, nullptr);
  ESP_LOGI(TAG, "discovery published for %s", s_deviceId.c_str());
}

// ============================================================
//  STATE
// ============================================================
static std::string buildState() {
  if (!audioStateLock(50)) return "";
  JsonPtr st(cJSON_CreateObject(), cJSON_Delete);
  cJSON* o = st.get();
  cJSON_AddNumberToObject(o, "volume", globalSettings.volume);
  const char* source = runtimeStatus.selectedMode == PLAYBACK_SD_CARD ? "SD Card"
                     : runtimeStatus.selectedMode == PLAYBACK_RADIO_URL ? "Radio" : "FTP";
  cJSON_AddStringToObject(o, "source", source);
  std::string station = (currentStation >= 0 && currentStation < (int)stations.size())
                            ? stations[currentStation].name : "-";
  cJSON_AddStringToObject(o, "station", station.c_str());
  bool playing = runtimeStatus.playbackState == STATE_PLAYING || runtimeStatus.playbackState == STATE_BUFFERING;
  cJSON_AddStringToObject(o, "play", playing ? "ON" : "OFF");
  std::string now = runtimeStatus.nowPlaying;
  if (runtimeStatus.selectedMode == PLAYBACK_SD_CARD) {
    size_t slash = now.find_last_of('/');
    if (slash != std::string::npos) now = now.substr(slash + 1);  // file name only
  }
  if (now.size() > 250) now.resize(250);  // HA sensor state limit is 255
  cJSON_AddStringToObject(o, "now_playing", now.empty() ? "-" : now.c_str());
  cJSON_AddStringToObject(o, "status", playbackStateLabel(runtimeStatus.playbackState));
  std::string stream = "-";
  if (audioDecoder.active() && audioDecoder.sampleRate() > 0) {
    stream = std::string(EspAudioDecoder::codecName(audioDecoder.codec())) + " " +
             std::to_string(audioDecoder.sampleRate()) + " Hz";
  }
  cJSON_AddStringToObject(o, "stream", stream.c_str());
  cJSON_AddStringToObject(o, "mono", soundSettings.mono ? "ON" : "OFF");
  cJSON_AddNumberToObject(o, "enh", soundSettings.enhancer);
  cJSON_AddNumberToObject(o, "hpf", soundSettings.protectHz);
  cJSON_AddNumberToObject(o, "nhz", soundSettings.notchHz);
  cJSON_AddNumberToObject(o, "ndb", soundSettings.notchDepthDb);
  audioStateUnlock();

  cJSON_AddNumberToObject(o, "rssi", s_rssi);
  cJSON_AddStringToObject(o, "ip", networkStaIp().c_str());

  char* text = cJSON_PrintUnformatted(o);
  std::string out = text ? text : "";
  cJSON_free(text);
  return out;
}

static void stateTask(void* arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(STATE_INTERVAL_MS));
    if (!s_connected) continue;
    xSemaphoreTake(s_clientLock, portMAX_DELAY);
    if (s_client == nullptr || !s_connected) {
      xSemaphoreGive(s_clientLock);
      continue;
    }

    if (s_needDiscovery) {
      s_needDiscovery = false;
      publishDiscovery();
      s_lastState.clear();
    }

    uint32_t now = millisNow();
    if (s_lastRssi == 0 || now - s_lastRssi > RSSI_INTERVAL_MS) {
      wifi_ap_record_t ap = {};
      if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
      s_lastRssi = now;
    }

    // Station list edited on the web page: refresh the select options.
    std::string names;
    for (const auto& n : stationNames()) names += n + "\n";
    if (names != s_lastStations) {
      std::vector<std::string> list = stationNames();
      announceStationSelect(list);
      s_lastStations = names;
    }

    std::string state = buildState();
    if (!state.empty() && state != s_lastState) {
      esp_mqtt_client_enqueue(s_client, (s_base + "/state").c_str(), state.c_str(), 0, 0, 1, true);
      s_lastState = state;
    }
    xSemaphoreGive(s_clientLock);
  }
}

// ============================================================
//  CLIENT
// ============================================================
static void onMqttEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  (void)base;
  auto* evt = static_cast<esp_mqtt_event_handle_t>(data);
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "connected to %s as %s", s_uri.c_str(), s_deviceId.c_str());
      s_connected = true;
      esp_mqtt_client_publish(s_client, (s_base + "/status").c_str(), "online", 0, 1, 1);
      esp_mqtt_client_subscribe(s_client, (s_base + "/set/+").c_str(), 1);
      esp_mqtt_client_subscribe(s_client, (s_base + "/cmd").c_str(), 1);
      esp_mqtt_client_subscribe(s_client, (std::string(DISCOVERY_PREFIX) + "/status").c_str(), 1);
      s_needDiscovery = true;
      break;
    case MQTT_EVENT_DISCONNECTED:
      if (s_connected) ESP_LOGW(TAG, "disconnected");
      s_connected = false;
      break;
    case MQTT_EVENT_DATA: {
      if (evt->current_data_offset != 0) break;  // ignore fragmented payloads
      std::string topic(evt->topic, evt->topic_len);
      std::string payload(evt->data, evt->data_len);
      if (topic == std::string(DISCOVERY_PREFIX) + "/status") {
        if (payload == "online") s_needDiscovery = true;  // HA restarted
      } else if (topic == s_base + "/cmd") {
        if (!remoteCommand(payload)) ESP_LOGW(TAG, "unknown command '%s'", payload.c_str());
      } else if (topic.compare(0, s_base.size() + 5, s_base + "/set/") == 0) {
        std::string key = topic.substr(s_base.size() + 5);
        if (!remoteSet(key, payload)) ESP_LOGW(TAG, "unknown setting '%s'", key.c_str());
      }
      s_lastState.clear();  // echo the new state promptly
      break;
    }
    case MQTT_EVENT_ERROR:
      if (evt->error_handle && evt->error_handle->connect_return_code != 0) {
        ESP_LOGW(TAG, "broker refused connection (code %d): check user/password",
                 (int)evt->error_handle->connect_return_code);
      }
      break;
    default:
      break;
  }
}

void mqttStop() {
  if (s_clientLock == nullptr) s_clientLock = xSemaphoreCreateMutex();
  xSemaphoreTake(s_clientLock, portMAX_DELAY);
  if (s_client == nullptr) {
    xSemaphoreGive(s_clientLock);
    return;
  }
  if (s_connected) {
    esp_mqtt_client_publish(s_client, (s_base + "/status").c_str(), "offline", 0, 1, 1);
  }
  s_connected = false;
  esp_mqtt_client_stop(s_client);
  esp_mqtt_client_destroy(s_client);
  s_client = nullptr;
  xSemaphoreGive(s_clientLock);
}

void mqttStart() {
  initIdentity();
  mqttStop();
  if (!mqttSettings.enabled || mqttSettings.host.empty()) {
    ESP_LOGI(TAG, "MQTT disabled");
    return;
  }

  static std::string lwtTopic;
  lwtTopic = s_base + "/status";
  s_uri = "mqtt://" + mqttSettings.host + ":" + std::to_string(mqttSettings.port);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = s_uri.c_str();
  cfg.credentials.client_id = s_deviceId.c_str();
  if (!mqttSettings.user.empty()) cfg.credentials.username = mqttSettings.user.c_str();
  if (!mqttSettings.password.empty()) cfg.credentials.authentication.password = mqttSettings.password.c_str();
  cfg.session.last_will.topic = lwtTopic.c_str();
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = 30;
  cfg.buffer.size = 2048;
  cfg.buffer.out_size = 4096;
  cfg.task.stack_size = 6144;
  cfg.network.reconnect_timeout_ms = 5000;

  s_client = esp_mqtt_client_init(&cfg);
  if (s_client == nullptr) {
    ESP_LOGE(TAG, "client init failed");
    return;
  }
  esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, onMqttEvent, nullptr);
  esp_mqtt_client_start(s_client);

  if (s_stateTask == nullptr) {
    xTaskCreatePinnedToCore(stateTask, "mqttState", 6144, nullptr, 3, &s_stateTask, 0);
  }
  ESP_LOGI(TAG, "connecting to %s (topic %s)", s_uri.c_str(), s_base.c_str());
}
