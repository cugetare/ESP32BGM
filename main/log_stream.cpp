// ============================================================
//  LOG STREAM
//  Copies every ESP_LOG line (still printed on the UART) into a
//  PSRAM ring buffer, served live over TCP (port LOG_TCP_PORT,
//  e.g. `nc <ip> 2323`) and through the /logs web page.
// ============================================================
#include "app_state.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const size_t LOG_RING_SIZE = 32 * 1024;
static const int    LOG_TCP_PORT = 2323;
static const int    LOG_MAX_CLIENTS = 2;

static char* s_ring = nullptr;
static volatile uint32_t s_total = 0;  // bytes ever written (monotonic)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prevVprintf = nullptr;

static void ringAppend(const char* data, size_t len) {
  portENTER_CRITICAL(&s_mux);
  uint32_t w = s_total;
  for (size_t i = 0; i < len; i++) s_ring[(w + i) % LOG_RING_SIZE] = data[i];
  s_total = w + len;
  portEXIT_CRITICAL(&s_mux);
}

static int logHook(const char* fmt, va_list args) {
  char line[256];
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (n > 0 && s_ring != nullptr) ringAppend(line, std::min((size_t)n, sizeof(line) - 1));
  return s_prevVprintf ? s_prevVprintf(fmt, args) : vprintf(fmt, args);
}

uint32_t logStreamPosition() {
  return s_total;
}

size_t logStreamRead(uint32_t& pos, char* out, size_t maxLen) {
  if (s_ring == nullptr) return 0;
  portENTER_CRITICAL(&s_mux);
  uint32_t total = s_total;
  if (total - pos > LOG_RING_SIZE || pos > total) {
    pos = total > LOG_RING_SIZE ? total - LOG_RING_SIZE : 0;  // fell behind: skip to oldest
  }
  size_t n = std::min((size_t)(total - pos), maxLen);
  for (size_t i = 0; i < n; i++) out[i] = s_ring[(pos + i) % LOG_RING_SIZE];
  pos += n;
  portEXIT_CRITICAL(&s_mux);
  return n;
}

uint32_t logStreamOldest() {
  uint32_t total = s_total;
  return total > LOG_RING_SIZE ? total - LOG_RING_SIZE : 0;
}

void logStreamInit() {
  if (s_ring != nullptr) return;
  s_ring = (char*)heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_ring == nullptr) return;
  s_prevVprintf = esp_log_set_vprintf(logHook);
}

// ============================================================
//  TCP SERVER
// ============================================================
struct LogClient {
  int fd = -1;
  uint32_t pos = 0;
};

static void closeClient(LogClient& c) {
  if (c.fd >= 0) close(c.fd);
  c.fd = -1;
}

static void tcpTask(void* arg) {
  (void)arg;
  int listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int yes = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LOG_TCP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (listenFd < 0 || bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd, 2) < 0) {
    ESP_LOGE("logtcp", "listen on %d failed", LOG_TCP_PORT);
    if (listenFd >= 0) close(listenFd);
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI("logtcp", "log stream on TCP port %d", LOG_TCP_PORT);

  LogClient clients[LOG_MAX_CLIENTS];
  char buf[1024];

  for (;;) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenFd, &readSet);
    int maxFd = listenFd;
    for (auto& c : clients) {
      if (c.fd >= 0) {
        FD_SET(c.fd, &readSet);
        maxFd = std::max(maxFd, c.fd);
      }
    }
    timeval tv = { 0, 200 * 1000 };
    int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &tv);

    if (ready > 0 && FD_ISSET(listenFd, &readSet)) {
      int fd = accept(listenFd, nullptr, nullptr);
      if (fd >= 0) {
        LogClient* slot = nullptr;
        for (auto& c : clients) if (c.fd < 0) { slot = &c; break; }
        if (slot == nullptr) {
          const char* busy = "too many log clients\r\n";
          send(fd, busy, strlen(busy), 0);
          close(fd);
        } else {
          slot->fd = fd;
          slot->pos = logStreamOldest();  // replay history first
        }
      }
    }

    for (auto& c : clients) {
      if (c.fd < 0) continue;
      if (ready > 0 && FD_ISSET(c.fd, &readSet)) {
        // Input is ignored; 0 or an error means the client went away.
        int r = recv(c.fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          closeClient(c);
          continue;
        }
      }
      uint32_t start = c.pos;
      size_t n = logStreamRead(c.pos, buf, sizeof(buf));
      if (n == 0) continue;
      int sent = send(c.fd, buf, n, MSG_DONTWAIT);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) c.pos = start;
        else closeClient(c);
      } else if ((size_t)sent < n) {
        c.pos = start + sent;
      }
    }
  }
}

void logStreamStartTcp() {
  static bool started = false;
  if (started || s_ring == nullptr) return;
  started = true;
  xTaskCreatePinnedToCore(tcpTask, "logTcp", 4096, nullptr, 2, nullptr, 0);
}
