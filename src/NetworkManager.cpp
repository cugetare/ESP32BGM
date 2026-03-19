#include "globals.h"
bool networkHasWifiCredentials() {
  return wifiSettings.ssid.length() > 0;
}

void networkStopAp() {
  if (!configApActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  configApActive = false;
  runtimeStatus.networkState = (WiFi.status() == WL_CONNECTED) ? NET_STA_CONNECTED : NET_OFF;
}

void networkStartConfigPortal() {
  if (configApActive) return;

  wifi_mode_t mode = (WiFi.status() == WL_CONNECTED) ? WIFI_AP_STA : WIFI_AP;
  WiFi.mode(mode);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  configApActive = true;
  runtimeStatus.networkState = (WiFi.status() == WL_CONNECTED) ? NET_STA_CONNECTED : NET_AP_ONLY;
}

bool networkEnsureRemoteAccess() {
  if (WiFi.status() == WL_CONNECTED) {
    runtimeStatus.networkState = NET_STA_CONNECTED;
    networkStopAp();
    return true;
  }

  if (!networkHasWifiCredentials()) {
    runtimeStatus.lastError = "WiFi credentials missing";
    networkStartConfigPortal();
    return false;
  }

  networkStopAp();
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSettings.ssid.c_str(), wifiSettings.password.c_str());
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  runtimeStatus.networkState = NET_STA_CONNECTING;

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_TIMEOUT_ATTEMPTS) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    runtimeStatus.networkState = NET_STA_CONNECTED;
    return true;
  }

  runtimeStatus.networkState = NET_ERROR;
  runtimeStatus.lastError = "WiFi connection failed";
  networkStartConfigPortal();
  return false;
}

bool networkEnsureOptionalAccess() {
  if (WiFi.status() == WL_CONNECTED) {
    runtimeStatus.networkState = NET_STA_CONNECTED;
    return true;
  }

  if (!networkHasWifiCredentials()) {
    runtimeStatus.networkState = configApActive ? NET_AP_ONLY : NET_OFF;
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSettings.ssid.c_str(), wifiSettings.password.c_str());
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  runtimeStatus.networkState = NET_STA_CONNECTING;

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_TIMEOUT_ATTEMPTS) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    runtimeStatus.networkState = NET_STA_CONNECTED;
    return true;
  }

  runtimeStatus.networkState = configApActive ? NET_AP_ONLY : NET_OFF;
  return false;
}

void networkBegin() {
  runtimeStatus.networkState = NET_OFF;

  if (modeRequiresNetwork(globalSettings.selectedMode)) {
    networkEnsureRemoteAccess();
    return;
  }

  networkEnsureOptionalAccess();
}

void networkLoop() {
  if (configApActive) {
    dnsServer.processNextRequest();
  }

  if (WiFi.status() == WL_CONNECTED) {
    runtimeStatus.networkState = NET_STA_CONNECTED;
  } else if (configApActive) {
    runtimeStatus.networkState = NET_AP_ONLY;
  } else if (runtimeStatus.networkState != NET_STA_CONNECTING) {
    runtimeStatus.networkState = NET_OFF;
  }
}
