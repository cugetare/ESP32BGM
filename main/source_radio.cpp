// ============================================================
//  RADIO URL SOURCE
//  Each stream gets its own reader task that pulls the HTTP body
//  into a PSRAM stream buffer. The audio task only drains that
//  buffer, so network stalls never block decoding or the web UI.
// ============================================================
#include "app_state.h"

#include <atomic>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/stream_buffer.h"

static const char* TAG = "radio";

static const uint32_t HEADER_WAIT_MS = 8000;
static const size_t   PLAYLIST_MAX_BYTES = 4096;
static const int      HTTP_READ_SIZE = 4096;

struct RadioSession {
  std::string url;
  std::string contentType;
  StreamBufferHandle_t buffer = nullptr;
  StaticStreamBuffer_t bufferStruct;
  uint8_t* storage = nullptr;
  std::atomic<int> refs{2};          // owner + reader task
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> headersReady{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> finished{false};
  int httpStatus = 0;
};

static RadioSession* s_session = nullptr;
static bool s_buffering = true;
static uint32_t s_lastAudioData = 0;
static uint32_t s_lastReconnectAttempt = 0;
static int s_recoveryCount = 0;
static uint8_t* s_chunk = nullptr;

static void sessionRelease(RadioSession* session) {
  if (session->refs.fetch_sub(1) == 1) {
    if (session->buffer != nullptr) vStreamBufferDelete(session->buffer);
    heap_caps_free(session->storage);
    delete session;
  }
}

static esp_err_t httpEvent(esp_http_client_event_t* evt) {
  auto* session = static_cast<RadioSession*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER && session != nullptr &&
      strcasecmp(evt->header_key, "Content-Type") == 0) {
    session->contentType = evt->header_value;
  }
  return ESP_OK;
}

static void readerTask(void* arg) {
  auto* session = static_cast<RadioSession*>(arg);
  uint8_t* buf = (uint8_t*)heap_caps_malloc(HTTP_READ_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  esp_http_client_config_t cfg = {};
  cfg.url = session->url.c_str();
  cfg.timeout_ms = 5000;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 1024;
  cfg.user_agent = "ESP32BGM/1.0";
  cfg.event_handler = httpEvent;
  cfg.user_data = session;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.keep_alive_enable = true;
  esp_http_client_handle_t client = (buf != nullptr) ? esp_http_client_init(&cfg) : nullptr;

  bool opened = false;
  if (client != nullptr) {
    // No ICY metadata: we want a clean audio byte stream.
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    for (int redirect = 0; redirect < 5 && !session->stopRequested; ++redirect) {
      if (esp_http_client_open(client, 0) != ESP_OK) break;
      esp_http_client_fetch_headers(client);
      session->httpStatus = esp_http_client_get_status_code(client);
      int s = session->httpStatus;
      if (s == 301 || s == 302 || s == 303 || s == 307 || s == 308) {
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        session->contentType.clear();
        continue;
      }
      opened = (s >= 200 && s < 300);
      break;
    }
  }

  if (!opened) {
    ESP_LOGW(TAG, "open failed (HTTP %d): %s", session->httpStatus, session->url.c_str());
    session->failed = true;
  } else {
    ESP_LOGI(TAG, "connected, Content-Type: %s", session->contentType.c_str());
  }
  session->headersReady = true;

  while (opened && !session->stopRequested) {
    int n = esp_http_client_read(client, (char*)buf, HTTP_READ_SIZE);
    if (n > 0) {
      size_t sent = 0;
      while (sent < (size_t)n && !session->stopRequested) {
        sent += xStreamBufferSend(session->buffer, buf + sent, n - sent, pdMS_TO_TICKS(100));
      }
      continue;
    }
    if (n == -ESP_ERR_HTTP_EAGAIN) continue;  // read timeout, keep waiting
    if (n == 0 && esp_http_client_is_complete_data_received(client)) {
      session->finished = true;
    } else {
      ESP_LOGW(TAG, "stream read ended (%d)", n);
      session->failed = true;
    }
    break;
  }

  if (client != nullptr) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
  heap_caps_free(buf);
  sessionRelease(session);
  vTaskDelete(nullptr);
}

static RadioSession* sessionStart(const std::string& url) {
  auto* session = new RadioSession();
  session->url = url;
  session->storage = (uint8_t*)heap_caps_malloc(RADIO_BUFFER_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (session->storage != nullptr) {
    session->buffer = xStreamBufferCreateStatic(RADIO_BUFFER_SIZE, 1, session->storage,
                                                &session->bufferStruct);
  }
  if (session->buffer == nullptr) {
    heap_caps_free(session->storage);
    delete session;
    return nullptr;
  }
  if (xTaskCreatePinnedToCore(readerTask, "radioRx", 8192, session, 5, nullptr, 0) != pdPASS) {
    vStreamBufferDelete(session->buffer);
    heap_caps_free(session->storage);
    delete session;
    return nullptr;
  }
  return session;
}

// Owner side: detach without waiting; the reader exits on its own.
static void sessionStop(RadioSession*& session) {
  if (session == nullptr) return;
  session->stopRequested = true;
  sessionRelease(session);
  session = nullptr;
}

static bool sessionWaitHeaders(RadioSession* session) {
  uint32_t start = millisNow();
  while (!session->headersReady && millisNow() - start < HEADER_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return session->headersReady && !session->failed;
}

// Reads an .m3u/.pls body and returns the first stream URL in it.
static std::string firstUrlFromPlaylist(RadioSession* session) {
  std::string body;
  uint32_t start = millisNow();
  char tmp[512];
  while (body.size() < PLAYLIST_MAX_BYTES && millisNow() - start < HEADER_WAIT_MS) {
    size_t n = xStreamBufferReceive(session->buffer, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
    if (n > 0) {
      body.append(tmp, n);
    } else if (session->finished || session->failed) {
      break;
    }
  }

  if (body.find("#EXT-X-") != std::string::npos) return "";  // HLS segment list

  size_t pos = 0;
  while (pos < body.size()) {
    size_t eol = body.find_first_of("\r\n", pos);
    std::string line = body.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = (eol == std::string::npos) ? body.size() : eol + 1;
    size_t http = line.find("http://");
    if (http == std::string::npos) http = line.find("https://");
    if (http != std::string::npos && line[0] != '#') {
      std::string url = line.substr(http);
      while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
      return url;
    }
  }
  return "";
}

bool radioBegin() {
  loadStations();
  if (stations.empty()) {
    runtimeStatus.lastError = "No stations configured";
    return false;
  }
  if (currentStation < 0 || currentStation >= (int)stations.size()) currentStation = 0;
  if (s_chunk == nullptr) {
    s_chunk = (uint8_t*)heap_caps_malloc(AUDIO_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (s_chunk == nullptr) {
    runtimeStatus.lastError = "Out of memory";
    return false;
  }

  s_recoveryCount = 0;
  settingsSaveRadio();
  return true;
}

void radioEnd() {
  sessionStop(s_session);
}

bool radioSelectStation(int index) {
  if (index < 0 || index >= (int)stations.size()) return false;
  currentStation = index;
  s_recoveryCount = 0;
  settingsSaveRadio();
  return true;
}

bool radioOpenSelected() {
  if (stations.empty()) return false;
  if (currentStation < 0 || currentStation >= (int)stations.size()) currentStation = 0;

  radioEnd();
  audioDecoder.end();

  const Station& station = stations[currentStation];
  std::string url = station.url;

  for (int hop = 0; hop < 2; ++hop) {
    s_session = sessionStart(url);
    if (s_session == nullptr) {
      runtimeStatus.lastError = "Out of memory for radio buffer";
      return false;
    }
    if (!sessionWaitHeaders(s_session)) {
      runtimeStatus.lastError = "Failed to open radio stream: " + url;
      radioEnd();
      return false;
    }
    if (!mimeIsPlaylist(s_session->contentType)) break;

    std::string next = firstUrlFromPlaylist(s_session);
    radioEnd();
    if (next.empty()) {
      runtimeStatus.lastError = "Playlist has no playable stream URL (HLS is not supported)";
      return false;
    }
    ESP_LOGI(TAG, "playlist -> %s", next.c_str());
    url = next;
  }
  if (s_session == nullptr) {
    runtimeStatus.lastError = "Nested playlists are not supported";
    return false;
  }

  std::string resolvedMime = station.mime.empty() ? s_session->contentType : station.mime;
  CodecHint codec = codecHintFromMime(resolvedMime);
  if (codec == CODEC_UNKNOWN) codec = codecHintFromPath(url);  // e.g. .../stream.flac
  if (codec == CODEC_UNKNOWN) {
    runtimeStatus.lastError = "Unsupported radio MIME: " + resolvedMime;
    radioEnd();
    return false;
  }

  SourceOpenResult out;
  out.codec = codec;
  out.title = station.name;
  out.isLive = true;
  if (!startDecodedStream(out)) {
    radioEnd();
    return false;
  }

  s_buffering = true;
  s_lastAudioData = millisNow();
  settingsSaveRadio();
  return true;
}

bool radioHandleFailure() {
  s_recoveryCount++;
  s_lastReconnectAttempt = millisNow();
  ESP_LOGW(TAG, "recovering stream (attempt %d)", s_recoveryCount);

  if (radioOpenSelected()) return true;

  if (s_recoveryCount >= RADIO_MAX_RECOVERIES) {
    runtimeStatus.playbackState = STATE_ERROR;
    runtimeStatus.lastError = "Radio recovery limit reached";
  }
  return false;
}

bool radioLoop() {
  if (runtimeStatus.playbackState == STATE_PAUSED ||
      runtimeStatus.playbackState == STATE_STOPPED ||
      runtimeStatus.playbackState == STATE_ERROR) {
    return false;
  }

  uint32_t now = millisNow();
  bool canRetry = now - s_lastReconnectAttempt > RECONNECT_INTERVAL_MS;

  if (!networkIsConnected()) {
    runtimeStatus.networkState = NET_ERROR;
    if (canRetry) radioHandleFailure();
    return false;
  }
  runtimeStatus.networkState = NET_STA_CONNECTED;

  if (s_session == nullptr) {
    if (canRetry) radioHandleFailure();
    return false;
  }

  bool ended = s_session->failed || s_session->finished;
  size_t available = xStreamBufferBytesAvailable(s_session->buffer);

  // Pre-buffer (and re-buffer after an underrun) to ride out Wi-Fi jitter.
  if (s_buffering) {
    if (available < RADIO_PREBUFFER_BYTES && !ended) {
      runtimeStatus.playbackState = STATE_BUFFERING;
      if (now - s_lastAudioData > STREAM_TIMEOUT_MS && canRetry) radioHandleFailure();
      return false;
    }
    s_buffering = false;
  }

  size_t n = xStreamBufferReceive(s_session->buffer, s_chunk, AUDIO_CHUNK_SIZE, 0);
  if (n > 0) {
    audioDecoder.write(s_chunk, n);
    s_lastAudioData = now;
    s_recoveryCount = 0;
    runtimeStatus.playbackState = STATE_PLAYING;
    return true;
  }

  if (ended) {
    if (canRetry) radioHandleFailure();
    return false;
  }

  s_buffering = true;
  runtimeStatus.playbackState = STATE_BUFFERING;
  return false;
}
