#include "globals.h"
void settingsLoadWifi() {
  prefs.begin("radio", true);
  wifiSettings.ssid = prefs.getString("wifi_ssid", "");
  wifiSettings.password = prefs.getString("wifi_pass", "");
  prefs.end();
}

void settingsSaveWifi(const String& ssid, const String& password) {
  prefs.begin("radio", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", password);
  prefs.end();
  wifiSettings.ssid = ssid;
  wifiSettings.password = password;
}

void settingsClearWifi() {
  prefs.begin("radio", false);
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  prefs.end();
  wifiSettings.ssid = "";
  wifiSettings.password = "";
}

void settingsLoadAll() {
  prefs.begin("radio", true);
  globalSettings.selectedMode = (PlaybackMode)prefs.getInt("mode", PLAYBACK_RADIO_URL);
  globalSettings.volume = constrain(prefs.getInt("volume", 50), 0, 100);
  globalSettings.bass = constrain(prefs.getInt("bass", 0), -10, 10);
  globalSettings.treble = constrain(prefs.getInt("treble", 0), -10, 10);
  currentStation = prefs.getInt("station", 0);
  currentSDTrack = prefs.getInt("sd_track", 0);
  wifiSettings.ssid = prefs.getString("wifi_ssid", "");
  wifiSettings.password = prefs.getString("wifi_pass", "");
  ftpSettings.host = prefs.getString("ftp_host", "");
  ftpSettings.port = prefs.getUInt("ftp_port", 21);
  ftpSettings.user = prefs.getString("ftp_user", "");
  ftpSettings.password = prefs.getString("ftp_pass", "");
  ftpSettings.basePath = prefs.getString("ftp_path", "/");
  ftpSettings.currentIndex = prefs.getInt("ftp_index", 0);
  mqttSettings.enabled = prefs.getBool("mqtt_en", false);
  mqttSettings.host = prefs.getString("mqtt_host", "");
  mqttSettings.port = prefs.getUInt("mqtt_port", 1883);
  mqttSettings.user = prefs.getString("mqtt_user", "");
  mqttSettings.password = prefs.getString("mqtt_pass", "");
  mqttSettings.baseTopic = prefs.getString("mqtt_topic", "advanced_webradio");
  prefs.end();

  runtimeStatus.selectedMode = globalSettings.selectedMode;
}

void settingsSaveGlobal() {
  prefs.begin("radio", false);
  prefs.putInt("mode", (int)globalSettings.selectedMode);
  prefs.putInt("volume", globalSettings.volume);
  prefs.putInt("bass", globalSettings.bass);
  prefs.putInt("treble", globalSettings.treble);
  prefs.end();
}

void settingsSaveRadio() {
  prefs.begin("radio", false);
  prefs.putInt("station", currentStation);
  prefs.end();
}

void settingsSaveSd() {
  prefs.begin("radio", false);
  prefs.putInt("sd_track", currentSDTrack);
  prefs.end();
}

// ============================================================
//  STATION STORAGE
// ============================================================
void loadStations() {
  stations.clear();

  if (!LittleFS.exists(STATIONS_FILE)) {
    Serial.println("No station file found, creating defaults");
    stations.push_back({ "Radio Paradise - Mellow", "http://stream.radioparadise.com/mellow-192", "audio/mpeg" });
    stations.push_back({ "Radio Paradise - Main", "http://stream.radioparadise.com/aac-320", "audio/aac" });
    stations.push_back({ "Radio Paradise - Rock", "http://stream.radioparadise.com/rock-320", "audio/aac" });
    stations.push_back({ "SomaFM - Groove Salad", "http://ice1.somafm.com/groovesalad-256-mp3", "audio/mpeg" });
    stations.push_back({ "SomaFM - Drone Zone", "http://ice1.somafm.com/dronezone-256-mp3", "audio/mpeg" });
    stations.push_back({ "SomaFM - Lush", "http://ice1.somafm.com/lush-128-mp3", "audio/mpeg" });
    saveStations();
    return;
  }

  File f = LittleFS.open(STATIONS_FILE, "r");
  if (!f) {
    Serial.println("Failed to open station file");
    return;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, f)) {
    Serial.println("Failed to parse station JSON");
    f.close();
    return;
  }
  f.close();

  for (JsonObject obj : doc["stations"].as<JsonArray>()) {
    stations.push_back({ obj["name"].as<String>(),
                         obj["url"].as<String>(),
                         obj["mime"] | "" });
  }
  Serial.printf("Stations loaded: %d\n", (int)stations.size());
}

void saveStations() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("stations");
  for (const auto& s : stations) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = s.name;
    obj["url"] = s.url;
    obj["mime"] = s.mime;
  }
  File f = LittleFS.open(STATIONS_FILE, "w");
  if (!f) {
    Serial.println("Failed to save station file");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.printf("Stations saved: %d\n", (int)stations.size());
}
