// ============================================================
//  NETWORK MANAGER
//  Wi-Fi STA with auto-reconnect, plus a setup AP with captive DNS
//  ("ESP32-Radio-Setup") when there are no credentials or the
//  connection fails.
// ============================================================
#include "app_state.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char* TAG = "net";

static const EventBits_t BIT_CONNECTED = BIT0;

static EventGroupHandle_t s_events = nullptr;
static esp_netif_t* s_staNetif = nullptr;
static esp_netif_t* s_apNetif = nullptr;
static bool s_wifiStarted = false;
static volatile bool s_wantSta = false;
static volatile bool s_apActive = false;
static char s_staIp[16] = "";
static wifi_config_t s_staConfig = {};
static wifi_config_t s_apConfig = {};

// Setup AP subnet. 192.168.4.x (the IDF default) is a common home LAN range;
// overlapping it with the STA network breaks routing while both are up.
static const uint8_t AP_IP[4] = { 192, 168, 71, 1 };

static void onWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (s_wantSta) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    s_staIp[0] = '\0';
    if (s_wantSta) esp_wifi_connect();  // keep retrying in the background
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* evt = static_cast<ip_event_got_ip_t*>(data);
    snprintf(s_staIp, sizeof(s_staIp), IPSTR, IP2STR(&evt->ip_info.ip));
    ESP_LOGI(TAG, "connected, IP %s", s_staIp);
    xEventGroupSetBits(s_events, BIT_CONNECTED);
  }
}

void networkInit() {
  if (s_events != nullptr) return;
  s_events = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_staNetif = esp_netif_create_default_wifi_sta();
  s_apNetif = esp_netif_create_default_wifi_ap();
  esp_netif_set_hostname(s_staNetif, "esp32bgm");

  esp_netif_ip_info_t apIp = {};
  IP4_ADDR(&apIp.ip, AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  IP4_ADDR(&apIp.gw, AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  IP4_ADDR(&apIp.netmask, 255, 255, 255, 0);
  esp_netif_dhcps_stop(s_apNetif);
  esp_netif_set_ip_info(s_apNetif, &apIp);
  esp_netif_dhcps_start(s_apNetif);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // credentials live in our own NVS keys
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr));
}

bool networkHasWifiCredentials() {
  return !wifiSettings.ssid.empty();
}

bool networkIsConnected() {
  return s_events != nullptr && (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool networkConfigPortalActive() {
  return s_apActive;
}

std::string networkStaIp() {
  return s_staIp;
}

static void refreshNetworkState() {
  if (networkIsConnected()) runtimeStatus.networkState = NET_STA_CONNECTED;
  else if (s_apActive) runtimeStatus.networkState = NET_AP_ONLY;
  else if (s_wantSta) runtimeStatus.networkState = NET_STA_CONNECTING;
  else runtimeStatus.networkState = NET_OFF;
}

static void applyMode() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (s_wantSta && s_apActive) mode = WIFI_MODE_APSTA;
  else if (s_wantSta) mode = WIFI_MODE_STA;
  else if (s_apActive) mode = WIFI_MODE_AP;

  if (mode == WIFI_MODE_NULL) {
    if (s_wifiStarted) {
      esp_wifi_stop();
      s_wifiStarted = false;
    }
    return;
  }

  // Configs go in before esp_wifi_start(): STA_START triggers the first
  // connect, which would otherwise run without an SSID.
  esp_wifi_set_mode(mode);
  if (s_wantSta) esp_wifi_set_config(WIFI_IF_STA, &s_staConfig);
  if (s_apActive) esp_wifi_set_config(WIFI_IF_AP, &s_apConfig);
  if (!s_wifiStarted) {
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);  // power save adds latency spikes to streams
    s_wifiStarted = true;
  }
}

void networkStopAp() {
  if (!s_apActive) return;
  captiveDnsStop();
  s_apActive = false;
  applyMode();
  refreshNetworkState();
}

void networkStartConfigPortal() {
  if (s_apActive) return;

  wifi_config_t& ap = s_apConfig;
  ap = {};
  strlcpy((char*)ap.ap.ssid, CONFIG_AP_SSID, sizeof(ap.ap.ssid));
  strlcpy((char*)ap.ap.password, CONFIG_AP_PASSWORD, sizeof(ap.ap.password));
  ap.ap.ssid_len = strlen(CONFIG_AP_SSID);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

  s_apActive = true;
  applyMode();

  esp_netif_ip_info_t ipInfo = {};
  esp_netif_get_ip_info(s_apNetif, &ipInfo);
  captiveDnsStart(ipInfo.ip.addr);
  ESP_LOGI(TAG, "setup AP \"%s\" at " IPSTR, CONFIG_AP_SSID, IP2STR(&ipInfo.ip));
  refreshNetworkState();
}

static bool connectSta() {
  wifi_config_t& sta = s_staConfig;
  sta = {};
  strlcpy((char*)sta.sta.ssid, wifiSettings.ssid.c_str(), sizeof(sta.sta.ssid));
  strlcpy((char*)sta.sta.password, wifiSettings.password.c_str(), sizeof(sta.sta.password));
  sta.sta.threshold.authmode = wifiSettings.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
  sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

  bool wasStarted = s_wifiStarted;
  s_wantSta = true;
  applyMode();
  runtimeStatus.networkState = NET_STA_CONNECTING;
  if (wasStarted) {
    esp_wifi_disconnect();  // the disconnect event reconnects with the new config
    esp_wifi_connect();
  }

  EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE,
                                         pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
  return (bits & BIT_CONNECTED) != 0;
}

bool networkEnsureRemoteAccess() {
  if (networkIsConnected()) {
    runtimeStatus.networkState = NET_STA_CONNECTED;
    networkStopAp();
    return true;
  }

  if (!networkHasWifiCredentials()) {
    runtimeStatus.lastError = "WiFi credentials missing";
    networkStartConfigPortal();
    return false;
  }

  if (connectSta()) {
    networkStopAp();
    runtimeStatus.networkState = NET_STA_CONNECTED;
    return true;
  }

  // Keep the STA retrying in the background while the setup AP is up.
  runtimeStatus.lastError = "WiFi connection failed";
  networkStartConfigPortal();
  runtimeStatus.networkState = NET_ERROR;
  return false;
}

bool networkEnsureOptionalAccess() {
  if (networkIsConnected()) {
    runtimeStatus.networkState = NET_STA_CONNECTED;
    return true;
  }

  // Without credentials the web UI would be unreachable: offer the setup AP.
  if (!networkHasWifiCredentials()) {
    networkStartConfigPortal();
    return false;
  }

  bool connected = connectSta();
  refreshNetworkState();
  return connected;
}

void networkBegin() {
  runtimeStatus.networkState = NET_OFF;
  if (modeRequiresNetwork(globalSettings.selectedMode)) {
    networkEnsureRemoteAccess();
    return;
  }
  networkEnsureOptionalAccess();
}
