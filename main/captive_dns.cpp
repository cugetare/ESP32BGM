// ============================================================
//  CAPTIVE DNS
//  Answers every A query with the setup AP address so phones open
//  the Wi-Fi setup page automatically.
// ============================================================
#include "app_state.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char* TAG = "dns";
static const int DNS_PORT = 53;

static TaskHandle_t s_task = nullptr;
static volatile bool s_stop = false;
static uint32_t s_apIp = 0;

static void dnsTask(void* arg) {
  (void)arg;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket failed");
    s_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DNS_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  timeval tv = { 1, 0 };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind failed");
    close(sock);
    s_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  uint8_t pkt[512];
  while (!s_stop) {
    sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    int len = recvfrom(sock, pkt, sizeof(pkt) - 16, 0, (sockaddr*)&from, &fromLen);
    if (len < 12) continue;

    // Standard query with exactly one question only.
    bool isQuery = (pkt[2] & 0x80) == 0 && (pkt[2] & 0x78) == 0;
    uint16_t qdCount = (pkt[4] << 8) | pkt[5];
    if (!isQuery || qdCount != 1) continue;

    // Find the end of the question (QNAME + QTYPE + QCLASS).
    int pos = 12;
    while (pos < len && pkt[pos] != 0) pos += pkt[pos] + 1;
    pos += 5;
    if (pos > len) continue;

    pkt[2] = 0x81;  // response, recursion desired
    pkt[3] = 0x80;  // recursion available, no error
    pkt[6] = 0; pkt[7] = 1;   // ANCOUNT = 1
    pkt[8] = 0; pkt[9] = 0;   // NSCOUNT
    pkt[10] = 0; pkt[11] = 0; // ARCOUNT

    const uint8_t answer[] = {
      0xC0, 0x0C,              // pointer to QNAME
      0x00, 0x01, 0x00, 0x01,  // type A, class IN
      0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
      0x00, 0x04               // RDLENGTH
    };
    memcpy(pkt + pos, answer, sizeof(answer));
    memcpy(pkt + pos + sizeof(answer), &s_apIp, 4);  // already network byte order
    sendto(sock, pkt, pos + sizeof(answer) + 4, 0, (sockaddr*)&from, fromLen);
  }

  close(sock);
  s_task = nullptr;
  vTaskDelete(nullptr);
}

void captiveDnsStart(uint32_t apIp) {
  s_apIp = apIp;
  s_stop = false;  // also revives a task that was asked to stop but has not exited yet
  if (s_task != nullptr) return;
  xTaskCreate(dnsTask, "captiveDns", 3072, nullptr, 3, &s_task);
}

void captiveDnsStop() {
  s_stop = true;  // the task exits within its 1 s receive timeout
}
