#include "globals.h"
// File-scope helpers (static = not visible to other translation units)
// Previously wrapped in 'namespace web{}' which worked fine in Arduino IDE
// (single translation unit) but caused unresolved-symbol errors in PlatformIO.
// Making them static achieves the same encapsulation without a namespace.
namespace {
const char* SETUP_TEMPLATE = "/setup.html";
const char* ROOT_TEMPLATE = "/root.html";
const char* MANAGE_TEMPLATE = "/manage.html";
const char* SDINFO_TEMPLATE = "/sdinfo.html";
const char* DIAGNOSTICS_TEMPLATE = "/diagnostics.html";

String htmlEscape(const String& input) {
  String out;
  out.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String loadTemplateFile(const char* path) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.printf("Missing LittleFS template: %s\n", path);
    return "";
  }
  String html = file.readString();
  file.close();
  return html;
}

String fallbackPage(const String& title, const String& message) {
  return "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>" + htmlEscape(title) + "</title>"
         "<style>body{font-family:Arial,sans-serif;max-width:520px;margin:40px auto;padding:20px;background:#f0f0f0}"
         ".box{background:white;padding:24px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,.1)}"
         "h1{margin-top:0;color:#333}.info{background:#fff3cd;padding:12px;border-radius:6px;color:#664d03}</style>"
         "</head><body><div class='box'><h1>" + htmlEscape(title) + "</h1><div class='info'>"
         + htmlEscape(message) + "</div></div></body></html>";
}

String loadPageOrFallback(const char* path, const String& title) {
  String html = loadTemplateFile(path);
  if (html.length() == 0) {
    return fallbackPage(title, "The page is missing from LittleFS. Upload the data folder to LittleFS and reboot the device.");
  }
  return html;
}

void replaceToken(String& html, const char* token, const String& value) {
  html.replace(token, value);
}

String buildModeOptions() {
  String options;
  for (int i = PLAYBACK_RADIO_URL; i <= PLAYBACK_FTP_LIBRARY; ++i) {
    PlaybackMode mode = (PlaybackMode)i;
    String label = playbackModeLabel(mode);
    if (mode == PLAYBACK_FTP_LIBRARY) label += " (coming soon)";
    options += "<option value='" + String(i) + "'";
    if (mode == runtimeStatus.selectedMode) options += " selected";
    options += ">" + htmlEscape(label) + "</option>";
  }
  return options;
}

String buildRadioOptions() {
  String options;
  for (size_t i = 0; i < stations.size(); ++i) {
    options += "<option value='" + String(i) + "'";
    if ((int)i == currentStation) options += " selected";
    options += ">" + htmlEscape(stations[i].name) + "</option>";
  }
  return options;
}

String buildSdOptions() {
  String options;
  for (size_t i = 0; i < sdTracks.size(); ++i) {
    String label = sdTracks[i];
    if (label.startsWith(SD_LIBRARY_ROOT)) {
      label.remove(0, String(SD_LIBRARY_ROOT).length());
      if (label.startsWith("/")) label.remove(0, 1);
    } else if (label.startsWith("/")) {
      label.remove(0, 1);
    }
    options += "<option value='" + String(i) + "'";
    if ((int)i == currentSDTrack) options += " selected";
    options += ">" + htmlEscape(label) + "</option>";
  }
  return options;
}

String buildSourceSection() {
  if (runtimeStatus.selectedMode == PLAYBACK_RADIO_URL) {
    if (stations.empty()) return "<div class='grp'><p>No stations configured</p></div>";
    return "<div class='grp'><label>Station</label><select id='item-select' onchange='changeItem(this.value)'>"
           + buildRadioOptions()
           + "</select><button onclick=\"location.href='/manage'\">Manage stations</button></div>";
  }

  if (runtimeStatus.selectedMode == PLAYBACK_SD_CARD) {
    if (sdTracks.empty()) return "<div class='grp'><p>No audio files found on the SD card</p><button onclick='refreshLibrary()'>Rescan</button></div>";
    return "<div class='grp'><label>File</label><select id='item-select' onchange='changeItem(this.value)'>"
           + buildSdOptions()
           + "</select><button onclick='refreshLibrary()'>Rescan library</button></div>";
  }

  return "<div class='grp'><p>FTP mode will be added in the next step.</p></div>";
}

String buildStatusText() {
  String text = "Mode: " + playbackModeLabel(runtimeStatus.selectedMode)
                + " | Playback: " + playbackStateLabel(runtimeStatus.playbackState)
                + " | Network: " + networkStateLabel(runtimeStatus.networkState);
  if (runtimeStatus.lastError.length() > 0) {
    text += "<br><strong>Error:</strong> " + htmlEscape(runtimeStatus.lastError);
  }
  return text;
}

String buildSdFileList() {
  if (!sdMounted) return "<p style='color:#666'>The card is not mounted</p>";
  if (sdTracks.empty()) return "<p style='color:#666'>No audio files found</p>";

  String html;
  for (const auto& track : sdTracks) {
    String label = track;
    if (label.startsWith(SD_LIBRARY_ROOT)) {
      label.remove(0, String(SD_LIBRARY_ROOT).length());
      if (label.startsWith("/")) label.remove(0, 1);
    } else if (label.startsWith("/")) {
      label.remove(0, 1);
    }
    html += "<div style='padding:8px;margin:4px 0;background:white;border-radius:4px;border-left:3px solid #0066cc'>"
            + htmlEscape(label) + "</div>";
  }
  return html;
}

String buildNowPlayingLabel() {
  if (runtimeStatus.nowPlaying.length() > 0) return runtimeStatus.nowPlaying;
  return "Nothing selected";
}
}  // namespace

void registerRoutes() {
  server.on("/", handleRoot);
  server.on("/setup", handleWifiSetup);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.on("/resetwifi", handleResetWifi);
  server.on("/reboot", handleReboot);
  server.on("/setmode", HTTP_POST, handleSetMode);
  server.on("/play", HTTP_POST, handlePlay);
  server.on("/pause", HTTP_POST, handlePause);
  server.on("/refreshlibrary", HTTP_POST, handleRefreshLibrary);
  server.on("/selectitem", handleSelectItem);
  server.on("/setstation", handleSelectItem);
  server.on("/setvolume", handleSetVolume);
  server.on("/setbass", handleSetBass);
  server.on("/settreble", handleSetTreble);
  server.on("/savedefaults", handleSaveDefaults);
  server.on("/manage", handleManage);
  server.on("/getstations", handleGetStations);
  server.on("/addstation", handleAddStation);
  server.on("/deletestation", handleDeleteStation);
  server.on("/sdinfo", handleSDInfo);
  server.on("/diagnostics", handleDiagnostics);
  server.onNotFound(handleRoot);
}

void handleSetMode() {
  if (!server.hasArg("m")) {
    server.send(400, "text/plain", "Missing mode");
    return;
  }

  int modeValue = server.arg("m").toInt();
  if (modeValue < PLAYBACK_RADIO_URL || modeValue > PLAYBACK_FTP_LIBRARY) {
    server.send(400, "text/plain", "Invalid mode");
    return;
  }

  if (!playbackSelectMode((PlaybackMode)modeValue)) {
    server.send(500, "text/plain", runtimeStatus.lastError.length() ? runtimeStatus.lastError : "Mode switch failed");
    return;
  }

  server.send(200, "text/plain", "OK");
}

void handlePlay() {
  if (!playbackPlay()) {
    server.send(500, "text/plain", runtimeStatus.lastError.length() ? runtimeStatus.lastError : "Play failed");
    return;
  }
  server.send(200, "text/plain", "OK");
}

void handlePause() {
  playbackPause();
  server.send(200, "text/plain", "OK");
}

void handleRefreshLibrary() {
  if (!playbackRefreshLibrary()) {
    server.send(500, "text/plain", runtimeStatus.lastError.length() ? runtimeStatus.lastError : "Refresh failed");
    return;
  }
  server.send(200, "text/plain", "OK");
}

void handleSetVolume() {
  if (!server.hasArg("v")) {
    server.send(400, "text/plain", "Missing v");
    return;
  }
  playbackSetVolume(server.arg("v").toInt());
  server.send(200, "text/plain", "OK");
}

void handleSetBass() {
  if (!server.hasArg("v")) {
    server.send(400, "text/plain", "Missing v");
    return;
  }
  playbackSetBass(server.arg("v").toInt());
  server.send(200, "text/plain", "OK");
}

void handleSetTreble() {
  if (!server.hasArg("v")) {
    server.send(400, "text/plain", "Missing v");
    return;
  }
  playbackSetTreble(server.arg("v").toInt());
  server.send(200, "text/plain", "OK");
}

void handleSelectItem() {
  if (!server.hasArg("i") && !server.hasArg("s")) {
    server.send(400, "text/plain", "Missing index");
    return;
  }

  int index = server.hasArg("i") ? server.arg("i").toInt() : server.arg("s").toInt();
  if (!playbackSelectItem(index)) {
    server.send(400, "text/plain", runtimeStatus.lastError.length() ? runtimeStatus.lastError : "Invalid selection");
    return;
  }
  server.send(200, "text/plain", "OK");
}

void handleSaveDefaults() {
  if (!audioStateLock(500)) {
    server.send(503, "text/plain", "Audio engine busy");
    return;
  }
  settingsSaveGlobal();
  settingsSaveRadio();
  settingsSaveSd();
  audioStateUnlock();
  server.send(200, "text/plain", "Saved");
}

void handleResetWifi() {
  settingsClearWifi();
  networkStartConfigPortal();
  server.send(200, "text/plain", "WiFi reset");
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(500);
  ESP.restart();
}

void handleGetStations() {
  if (!audioStateLock(500)) {
    server.send(503, "text/plain", "Audio engine busy");
    return;
  }
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < stations.size(); ++i) {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = i;
    obj["name"] = stations[i].name;
    obj["url"] = stations[i].url;
    obj["mime"] = stations[i].mime;
  }
  String out;
  serializeJson(doc, out);
  audioStateUnlock();
  server.send(200, "application/json", out);
}

void handleAddStation() {
  if (!audioStateLock(1000)) {
    server.send(503, "text/plain", "Audio engine busy");
    return;
  }
  if (!server.hasArg("name") || !server.hasArg("url")) {
    audioStateUnlock();
    server.send(400, "text/plain", "Missing params");
    return;
  }

  String name = server.arg("name");
  String urlValue = server.arg("url");
  String mimeValue = server.hasArg("mime") ? server.arg("mime") : "";
  name.trim();
  urlValue.trim();
  mimeValue.trim();
  if (name.length() == 0 || urlValue.length() == 0) {
    audioStateUnlock();
    server.send(400, "text/plain", "Invalid params");
    return;
  }

  stations.push_back({ name, urlValue, mimeValue });
  saveStations();
  audioStateUnlock();
  server.send(200, "text/plain", "Added");
}

void handleDeleteStation() {
  if (!audioStateLock(1000)) {
    server.send(503, "text/plain", "Audio engine busy");
    return;
  }
  if (!server.hasArg("id")) {
    audioStateUnlock();
    server.send(400, "text/plain", "Missing id");
    return;
  }

  int id = server.arg("id").toInt();
  if (id < 0 || id >= (int)stations.size()) {
    audioStateUnlock();
    server.send(400, "text/plain", "Invalid id");
    return;
  }

  stations.erase(stations.begin() + id);
  if (currentStation >= (int)stations.size()) currentStation = 0;
  saveStations();
  settingsSaveRadio();
  audioStateUnlock();
  server.send(200, "text/plain", "Deleted");
}

void handleSaveWifi() {
  if (!server.hasArg("ssid") || !server.hasArg("password")) {
    server.send(400, "text/plain", "Missing params");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.arg("password");
  ssid.trim();
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID required");
    return;
  }

  settingsSaveWifi(ssid, password);
  bool connected = networkEnsureRemoteAccess();
  if (!audioStateLock(1000)) {
    server.send(503, "text/plain", "Audio engine busy");
    return;
  }
  loadStations();
  audioStateUnlock();
  if (connected && runtimeStatus.selectedMode == PLAYBACK_RADIO_URL) {
    modeSwitchTo(PLAYBACK_RADIO_URL);
  }
  server.send(200, "text/html", "<h2>WiFi saved</h2><p>Radio mode is ready. Return to the main page.</p>");
}

void handleSDInfo() {
  if (!audioStateLock(500)) {
    server.send(503, "text/plain", "Audio engine busy");
    return;
  }
  String cardType = "N/A";
  String cardSize = "N/A";

  if (sdMounted) {
    uint8_t type = SD_MMC.cardType();
    if (type == CARD_MMC) cardType = "MMC";
    else if (type == CARD_SD) cardType = "SDSC";
    else if (type == CARD_SDHC) cardType = "SDHC";
    cardSize = String((unsigned long)(SD_MMC.cardSize() / (1024ULL * 1024ULL))) + " MB";
  }

  String html = loadPageOrFallback(SDINFO_TEMPLATE, "SD Info");
  replaceToken(html, "{{SD_STATUS}}", sdMounted ? "Connected" : "Not detected");
  replaceToken(html, "{{CARD_TYPE}}", cardType);
  replaceToken(html, "{{CARD_SIZE}}", cardSize);
  replaceToken(html, "{{FILE_COUNT}}", String(sdTracks.size()));
  replaceToken(html, "{{FILE_LIST}}", buildSdFileList());
  audioStateUnlock();
  server.send(200, "text/html", html);
}

void handleWifiSetup() {
  String html = loadPageOrFallback(SETUP_TEMPLATE, "WiFi Setup");
  replaceToken(html, "{{WIFI_SSID}}", htmlEscape(wifiSettings.ssid));
  server.send(200, "text/html", html);
}

void handleRoot() {
  if (!audioStateLock(500)) {
    server.send(503, "text/plain", "Audio engine busy");
    return;
  }
  if (runtimeStatus.selectedMode == PLAYBACK_RADIO_URL && stations.empty()) {
    loadStations();
  }
  String html = loadPageOrFallback(ROOT_TEMPLATE, "ESP32 Audio");
  replaceToken(html, "{{NOW_PLAYING}}", htmlEscape(buildNowPlayingLabel()));
  replaceToken(html, "{{MODE_OPTIONS}}", buildModeOptions());
  replaceToken(html, "{{SOURCE_SECTION}}", buildSourceSection());
  replaceToken(html, "{{STATUS_TEXT}}", buildStatusText());
  replaceToken(html, "{{CURRENT_VOLUME}}", String(globalSettings.volume));
  replaceToken(html, "{{CURRENT_BASS}}", String(globalSettings.bass));
  replaceToken(html, "{{CURRENT_TREBLE}}", String(globalSettings.treble));
  audioStateUnlock();
  server.send(200, "text/html", html);
}

void handleManage() {
  String html = loadPageOrFallback(MANAGE_TEMPLATE, "Manage Stations");
  server.send(200, "text/html", html);
}

void handleDiagnostics() {
  String html = loadPageOrFallback(DIAGNOSTICS_TEMPLATE, "Diagnostics");
  replaceToken(html, "{{CHIP_TEMP}}", htmlEscape(formatChipTemperature()));
  replaceToken(html, "{{AUDIO_CORE}}", String((int)audioTaskCore));
  replaceToken(html, "{{WEB_CORE}}", String((int)xPortGetCoreID()));
  server.send(200, "text/html", html);
}
