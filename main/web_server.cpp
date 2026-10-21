// ============================================================
//  WEB SERVER (esp_http_server)
//  Same routes, parameters and HTML templates as the Arduino build;
//  the templates are embedded in the firmware (main/web/*.html).
// ============================================================
#include "app_state.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <memory>
#include <new>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"

static const char* TAG = "web";

extern const char root_html_start[] asm("_binary_root_html_start");
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char manage_html_start[] asm("_binary_manage_html_start");
extern const char sdinfo_html_start[] asm("_binary_sdinfo_html_start");
extern const char diagnostics_html_start[] asm("_binary_diagnostics_html_start");
extern const char update_html_start[] asm("_binary_update_html_start");
extern const char logs_html_start[] asm("_binary_logs_html_start");

static const size_t MAX_BODY_BYTES = 2048;
static int s_webCore = -1;

// ============================================================
//  HELPERS
// ============================================================
namespace {

std::string htmlEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (char c : input) {
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

std::string urlDecode(const char* in) {
  std::string out;
  for (const char* p = in; *p; ++p) {
    if (*p == '+') {
      out += ' ';
    } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      char hex[3] = { p[1], p[2], 0 };
      out += (char)strtol(hex, nullptr, 16);
      p += 2;
    } else {
      out += *p;
    }
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Query-string and x-www-form-urlencoded body arguments, like WebServer::arg().
struct RequestArgs {
  std::string query;
  std::string body;

  bool get(const char* key, std::string& out) const {
    char buf[1024];
    for (const std::string* src : { &query, &body }) {
      if (!src->empty() && httpd_query_key_value(src->c_str(), key, buf, sizeof(buf)) == ESP_OK) {
        out = urlDecode(buf);
        return true;
      }
    }
    return false;
  }

  bool has(const char* key) const {
    std::string ignored;
    return get(key, ignored);
  }

  int getInt(const char* key, int fallback = 0) const {
    std::string v;
    return get(key, v) ? atoi(v.c_str()) : fallback;
  }
};

RequestArgs readArgs(httpd_req_t* req) {
  RequestArgs args;
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0) {
    args.query.resize(qlen + 1);
    httpd_req_get_url_query_str(req, &args.query[0], qlen + 1);
    args.query.resize(qlen);
  }
  if (req->content_len > 0 && req->content_len <= MAX_BODY_BYTES) {
    args.body.resize(req->content_len);
    size_t got = 0;
    while (got < req->content_len) {
      int r = httpd_req_recv(req, &args.body[got], req->content_len - got);
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      if (r <= 0) break;
      got += r;
    }
    args.body.resize(got);
  }
  return args;
}

esp_err_t send(httpd_req_t* req, int code, const char* type, const std::string& body) {
  switch (code) {
    case 200: break;
    case 400: httpd_resp_set_status(req, "400 Bad Request"); break;
    case 500: httpd_resp_set_status(req, "500 Internal Server Error"); break;
    case 503: httpd_resp_set_status(req, "503 Service Unavailable"); break;
    default: httpd_resp_set_status(req, HTTPD_500); break;
  }
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t sendText(httpd_req_t* req, int code, const std::string& body) {
  return send(req, code, "text/plain", body);
}

esp_err_t sendHtml(httpd_req_t* req, const std::string& html) {
  return send(req, 200, "text/html; charset=utf-8", html);
}

const std::string& errorOr(const char* fallback) {
  static std::string tmp;
  tmp = runtimeStatus.lastError.empty() ? fallback : runtimeStatus.lastError;
  return tmp;
}

void replaceToken(std::string& html, const char* token, const std::string& value) {
  size_t tokenLen = strlen(token);
  size_t pos = 0;
  while ((pos = html.find(token, pos)) != std::string::npos) {
    html.replace(pos, tokenLen, value);
    pos += value.size();
  }
}

std::string trackLabel(const std::string& track) {
  std::string label = track;
  std::string root = SD_LIBRARY_ROOT;
  if (label.compare(0, root.size(), root) == 0) label.erase(0, root.size());
  if (!label.empty() && label[0] == '/') label.erase(0, 1);
  return label;
}

std::string buildModeOptions() {
  std::string options;
  for (int i = PLAYBACK_RADIO_URL; i <= PLAYBACK_FTP_LIBRARY; ++i) {
    PlaybackMode mode = (PlaybackMode)i;
    std::string label = playbackModeLabel(mode);
    if (mode == PLAYBACK_FTP_LIBRARY) label += " (coming soon)";
    options += "<option value='" + std::to_string(i) + "'";
    if (mode == runtimeStatus.selectedMode) options += " selected";
    options += ">" + htmlEscape(label) + "</option>";
  }
  return options;
}

std::string buildRadioOptions() {
  std::string options;
  for (size_t i = 0; i < stations.size(); ++i) {
    options += "<option value='" + std::to_string(i) + "'";
    if ((int)i == currentStation) options += " selected";
    options += ">" + htmlEscape(stations[i].name) + "</option>";
  }
  return options;
}

std::string buildSdOptions() {
  std::string options;
  for (size_t i = 0; i < sdTracks.size(); ++i) {
    options += "<option value='" + std::to_string(i) + "'";
    if ((int)i == currentSDTrack) options += " selected";
    options += ">" + htmlEscape(trackLabel(sdTracks[i])) + "</option>";
  }
  return options;
}

std::string buildSourceSection() {
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

std::string buildStatusText() {
  std::string text = std::string("Mode: ") + playbackModeLabel(runtimeStatus.selectedMode)
                     + " | Playback: " + playbackStateLabel(runtimeStatus.playbackState)
                     + " | Network: " + networkStateLabel(runtimeStatus.networkState);
  if (audioDecoder.active() && audioDecoder.sampleRate() > 0) {
    text += std::string(" | ") + EspAudioDecoder::codecName(audioDecoder.codec()) + " " +
            std::to_string(audioDecoder.sampleRate()) + " Hz";
  }
  if (!runtimeStatus.lastError.empty()) {
    text += "<br><strong>Error:</strong> " + htmlEscape(runtimeStatus.lastError);
  }
  return text;
}

std::string buildSdFileList() {
  if (!sdMounted) return "<p style='color:#666'>The card is not mounted</p>";
  if (sdTracks.empty()) return "<p style='color:#666'>No audio files found</p>";

  std::string html;
  for (const auto& track : sdTracks) {
    html += "<div style='padding:8px;margin:4px 0;background:white;border-radius:4px;border-left:3px solid #0066cc'>"
            + htmlEscape(trackLabel(track)) + "</div>";
  }
  return html;
}

}  // namespace

// ============================================================
//  HANDLERS
// ============================================================
static esp_err_t handleRoot(httpd_req_t* req) {
  if (!audioStateLock(500)) return sendText(req, 503, "Audio engine busy");
  if (runtimeStatus.selectedMode == PLAYBACK_RADIO_URL && stations.empty()) loadStations();

  std::string html = root_html_start;
  replaceToken(html, "{{NOW_PLAYING}}",
               htmlEscape(runtimeStatus.nowPlaying.empty() ? "Nothing selected" : runtimeStatus.nowPlaying));
  replaceToken(html, "{{MODE_OPTIONS}}", buildModeOptions());
  replaceToken(html, "{{SOURCE_SECTION}}", buildSourceSection());
  replaceToken(html, "{{STATUS_TEXT}}", buildStatusText());
  replaceToken(html, "{{CURRENT_VOLUME}}", std::to_string(globalSettings.volume));
  replaceToken(html, "{{CURRENT_BASS}}", std::to_string(globalSettings.bass));
  replaceToken(html, "{{CURRENT_TREBLE}}", std::to_string(globalSettings.treble));
  audioStateUnlock();
  return sendHtml(req, html);
}

static esp_err_t handleNotFound(httpd_req_t* req, httpd_err_code_t err) {
  (void)err;
  // Captive-portal probes (and any unknown path) land on the main page.
  return handleRoot(req);
}

static esp_err_t handleWifiSetup(httpd_req_t* req) {
  std::string html = setup_html_start;
  replaceToken(html, "{{WIFI_SSID}}", htmlEscape(wifiSettings.ssid));
  return sendHtml(req, html);
}

static esp_err_t handleSaveWifi(httpd_req_t* req) {
  RequestArgs args = readArgs(req);
  std::string ssid, password;
  if (!args.get("ssid", ssid) || !args.get("password", password)) {
    return sendText(req, 400, "Missing params");
  }
  ssid = trim(ssid);
  if (ssid.empty()) return sendText(req, 400, "SSID required");

  settingsSaveWifi(ssid, password);
  bool connected = networkEnsureRemoteAccess();

  if (audioStateLock(1000)) {
    loadStations();
    if (connected && runtimeStatus.selectedMode == PLAYBACK_RADIO_URL) {
      modeSwitchTo(PLAYBACK_RADIO_URL);
    }
    audioStateUnlock();
  }

  std::string msg = connected
      ? "<h2>WiFi saved</h2><p>Connected. Open http://" + networkStaIp() + "/ on your network.</p>"
      : "<h2>WiFi saved</h2><p>Could not connect yet; the device keeps retrying.</p>";
  return sendHtml(req, msg);
}

static esp_err_t handleResetWifi(httpd_req_t* req) {
  settingsClearWifi();
  networkStartConfigPortal();
  return sendText(req, 200, "WiFi reset");
}

static esp_err_t handleReboot(httpd_req_t* req) {
  sendText(req, 200, "Rebooting...");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

static esp_err_t handleSetMode(httpd_req_t* req) {
  RequestArgs args = readArgs(req);
  if (!args.has("m")) return sendText(req, 400, "Missing mode");

  int modeValue = args.getInt("m", -1);
  if (modeValue < PLAYBACK_RADIO_URL || modeValue > PLAYBACK_FTP_LIBRARY) {
    return sendText(req, 400, "Invalid mode");
  }
  if (!playbackSelectMode((PlaybackMode)modeValue)) {
    return sendText(req, 500, errorOr("Mode switch failed"));
  }
  return sendText(req, 200, "OK");
}

static esp_err_t handlePlay(httpd_req_t* req) {
  if (!playbackPlay()) return sendText(req, 500, errorOr("Play failed"));
  return sendText(req, 200, "OK");
}

static esp_err_t handlePause(httpd_req_t* req) {
  playbackPause();
  return sendText(req, 200, "OK");
}

static esp_err_t handleRefreshLibrary(httpd_req_t* req) {
  if (!playbackRefreshLibrary()) return sendText(req, 500, errorOr("Refresh failed"));
  return sendText(req, 200, "OK");
}

static esp_err_t handleSelectItem(httpd_req_t* req) {
  RequestArgs args = readArgs(req);
  if (!args.has("i") && !args.has("s")) return sendText(req, 400, "Missing index");

  int index = args.has("i") ? args.getInt("i") : args.getInt("s");
  if (!playbackSelectItem(index)) return sendText(req, 400, errorOr("Invalid selection"));
  return sendText(req, 200, "OK");
}

static esp_err_t handleSetLevel(httpd_req_t* req, void (*setter)(int)) {
  RequestArgs args = readArgs(req);
  if (!args.has("v")) return sendText(req, 400, "Missing v");
  setter(args.getInt("v"));
  return sendText(req, 200, "OK");
}

static esp_err_t handleSetVolume(httpd_req_t* req) { return handleSetLevel(req, playbackSetVolume); }
static esp_err_t handleSetBass(httpd_req_t* req) { return handleSetLevel(req, playbackSetBass); }
static esp_err_t handleSetTreble(httpd_req_t* req) { return handleSetLevel(req, playbackSetTreble); }

static esp_err_t handleSaveDefaults(httpd_req_t* req) {
  if (!audioStateLock(500)) return sendText(req, 503, "Audio engine busy");
  settingsSaveGlobal();
  settingsSaveRadio();
  settingsSaveSd();
  audioStateUnlock();
  return sendText(req, 200, "Saved");
}

static esp_err_t handleManage(httpd_req_t* req) {
  return sendHtml(req, manage_html_start);
}

static esp_err_t handleGetStations(httpd_req_t* req) {
  if (!audioStateLock(500)) return sendText(req, 503, "Audio engine busy");
  cJSON* arr = cJSON_CreateArray();
  for (size_t i = 0; i < stations.size(); ++i) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", (double)i);
    cJSON_AddStringToObject(obj, "name", stations[i].name.c_str());
    cJSON_AddStringToObject(obj, "url", stations[i].url.c_str());
    cJSON_AddStringToObject(obj, "mime", stations[i].mime.c_str());
    cJSON_AddItemToArray(arr, obj);
  }
  audioStateUnlock();

  char* json = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  std::string out = json ? json : "[]";
  cJSON_free(json);
  return send(req, 200, "application/json", out);
}

static esp_err_t handleAddStation(httpd_req_t* req) {
  RequestArgs args = readArgs(req);
  std::string name, url, mime;
  if (!args.get("name", name) || !args.get("url", url)) return sendText(req, 400, "Missing params");
  args.get("mime", mime);
  name = trim(name);
  url = trim(url);
  mime = trim(mime);
  if (name.empty() || url.empty()) return sendText(req, 400, "Invalid params");

  if (!audioStateLock(1000)) return sendText(req, 503, "Audio engine busy");
  stations.push_back({ name, url, mime });
  saveStations();
  audioStateUnlock();
  return sendText(req, 200, "Added");
}

static esp_err_t handleDeleteStation(httpd_req_t* req) {
  RequestArgs args = readArgs(req);
  if (!args.has("id")) return sendText(req, 400, "Missing id");
  int id = args.getInt("id", -1);

  if (!audioStateLock(1000)) return sendText(req, 503, "Audio engine busy");
  if (id < 0 || id >= (int)stations.size()) {
    audioStateUnlock();
    return sendText(req, 400, "Invalid id");
  }
  stations.erase(stations.begin() + id);
  if (currentStation >= (int)stations.size()) currentStation = 0;
  saveStations();
  settingsSaveRadio();
  audioStateUnlock();
  return sendText(req, 200, "Deleted");
}

static esp_err_t handleSDInfo(httpd_req_t* req) {
  if (!audioStateLock(500)) return sendText(req, 503, "Audio engine busy");
  std::string cardType = "N/A";
  std::string cardSize = "N/A";
  uint64_t sizeBytes = 0;
  if (sdCardInfo(cardType, sizeBytes)) {
    cardSize = std::to_string((unsigned long long)(sizeBytes / (1024ULL * 1024ULL))) + " MB";
  }

  std::string html = sdinfo_html_start;
  replaceToken(html, "{{SD_STATUS}}", sdMounted ? "Connected" : "Not detected");
  replaceToken(html, "{{CARD_TYPE}}", cardType);
  replaceToken(html, "{{CARD_SIZE}}", cardSize);
  replaceToken(html, "{{FILE_COUNT}}", std::to_string(sdTracks.size()));
  replaceToken(html, "{{FILE_LIST}}", buildSdFileList());
  audioStateUnlock();
  return sendHtml(req, html);
}

static esp_err_t handleDiagnostics(httpd_req_t* req) {
  std::string html = diagnostics_html_start;
  // The original ESP32 has no supported on-chip temperature sensor in IDF 5.
  replaceToken(html, "{{CHIP_TEMP}}", "Unavailable");
  replaceToken(html, "{{AUDIO_CORE}}", std::to_string(audioTaskCore));
  replaceToken(html, "{{WEB_CORE}}", std::to_string(s_webCore >= 0 ? s_webCore : xPortGetCoreID()));
  return sendHtml(req, html);
}

static esp_err_t handleUpdatePage(httpd_req_t* req) {
  std::string html = update_html_start;
  replaceToken(html, "{{FW_INFO}}", otaRunningInfo());
  return sendHtml(req, html);
}

// A failed upload leaves the running firmware untouched: resume playback.
static esp_err_t failUpload(httpd_req_t* req, int code, const std::string& err) {
  otaAbort();
  ESP_LOGW(TAG, "OTA upload failed: %s", err.c_str());
  modeSwitchTo(runtimeStatus.selectedMode);  // full restart: SD mode was unmounted by the stop
  return sendText(req, code, err);
}

// Raw firmware image in the request body (application/octet-stream).
static esp_err_t handleUpdateUpload(httpd_req_t* req) {
  std::string err;
  if (req->content_len == 0) return sendText(req, 400, "Empty upload");

  playbackStop();  // flash writes stall the cache; don't fight the decoder for it
  if (!otaBegin(req->content_len, err)) return failUpload(req, 400, err);

  const size_t CHUNK = 4096;
  std::unique_ptr<char[]> buf(new (std::nothrow) char[CHUNK]);
  if (!buf) return failUpload(req, 500, "Out of memory");

  size_t remaining = req->content_len;
  int timeouts = 0;
  while (remaining > 0) {
    int r = httpd_req_recv(req, buf.get(), std::min(remaining, CHUNK));
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
    if (r <= 0) return failUpload(req, 500, "Upload interrupted");
    timeouts = 0;
    if (!otaWrite(buf.get(), r, err)) return failUpload(req, 400, err);
    remaining -= r;
  }

  if (!otaFinish(err)) return failUpload(req, 400, err);
  return sendText(req, 200, "OK, rebooting");
}

static esp_err_t handleLogsPage(httpd_req_t* req) {
  std::string html = logs_html_start;
  std::string ip = networkStaIp();
  replaceToken(html, "{{DEVICE_IP}}", ip.empty() ? "&lt;device-ip&gt;" : ip);
  return sendHtml(req, html);
}

// Log text since ?from=<pos> (whole buffer without it); next position in X-Log-Pos.
static esp_err_t handleLogTail(httpd_req_t* req) {
  RequestArgs args = readArgs(req);
  std::string from;
  uint32_t pos = args.get("from", from) ? (uint32_t)strtoul(from.c_str(), nullptr, 10)
                                        : logStreamOldest();
  std::string body;
  char buf[2048];
  size_t n;
  while (body.size() < 32 * 1024 && (n = logStreamRead(pos, buf, sizeof(buf))) > 0) {
    body.append(buf, n);
  }
  char posHeader[16];
  snprintf(posHeader, sizeof(posHeader), "%lu", (unsigned long)pos);
  httpd_resp_set_hdr(req, "X-Log-Pos", posHeader);
  return send(req, 200, "text/plain; charset=utf-8", body);
}

// ============================================================
//  SERVER
// ============================================================
struct Route {
  const char* uri;
  esp_err_t (*handler)(httpd_req_t*);
  bool get;
  bool post;
};

static const Route ROUTES[] = {
  { "/",               handleRoot,           true,  true  },
  { "/setup",          handleWifiSetup,      true,  true  },
  { "/savewifi",       handleSaveWifi,       false, true  },
  { "/resetwifi",      handleResetWifi,      true,  true  },
  { "/reboot",         handleReboot,         true,  true  },
  { "/setmode",        handleSetMode,        false, true  },
  { "/play",           handlePlay,           false, true  },
  { "/pause",          handlePause,          false, true  },
  { "/refreshlibrary", handleRefreshLibrary, false, true  },
  { "/selectitem",     handleSelectItem,     true,  true  },
  { "/setstation",     handleSelectItem,     true,  true  },
  { "/setvolume",      handleSetVolume,      true,  true  },
  { "/setbass",        handleSetBass,        true,  true  },
  { "/settreble",      handleSetTreble,      true,  true  },
  { "/savedefaults",   handleSaveDefaults,   true,  true  },
  { "/manage",         handleManage,         true,  true  },
  { "/getstations",    handleGetStations,    true,  true  },
  { "/addstation",     handleAddStation,     true,  true  },
  { "/deletestation",  handleDeleteStation,  true,  true  },
  { "/sdinfo",         handleSDInfo,         true,  true  },
  { "/diagnostics",    handleDiagnostics,    true,  true  },
  { "/logs",           handleLogsPage,       true,  false },
  { "/logtail",        handleLogTail,        true,  false },
};

bool startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 10240;
  config.max_uri_handlers = 48;
  config.lru_purge_enable = true;
  config.core_id = 0;
  config.task_priority = 4;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server start failed");
    return false;
  }
  s_webCore = config.core_id;

  for (const Route& r : ROUTES) {
    for (int m = 0; m < 2; ++m) {
      bool enabled = (m == 0) ? r.get : r.post;
      if (!enabled) continue;
      httpd_uri_t uri = {};
      uri.uri = r.uri;
      uri.method = (m == 0) ? HTTP_GET : HTTP_POST;
      uri.handler = r.handler;
      httpd_register_uri_handler(server, &uri);
    }
  }
  // /update shares a URI between the page (GET) and the upload (POST).
  httpd_uri_t updatePage = {};
  updatePage.uri = "/update";
  updatePage.method = HTTP_GET;
  updatePage.handler = handleUpdatePage;
  httpd_register_uri_handler(server, &updatePage);
  httpd_uri_t updateUpload = {};
  updateUpload.uri = "/update";
  updateUpload.method = HTTP_POST;
  updateUpload.handler = handleUpdateUpload;
  httpd_register_uri_handler(server, &updateUpload);

  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handleNotFound);
  ESP_LOGI(TAG, "HTTP server started");
  return true;
}
