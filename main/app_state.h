#pragma once

// ============================================================
//  SHARED TYPES, GLOBALS AND CROSS-MODULE DECLARATIONS
//  (ESP-IDF port of the Arduino globals.h)
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_decoder.h"

// ============================================================
//  TYPES
// ============================================================

struct Station {
  std::string name;
  std::string url;
  std::string mime;
};

enum PlaybackMode {
  PLAYBACK_RADIO_URL = 0,
  PLAYBACK_SD_CARD = 1,
  PLAYBACK_FTP_LIBRARY = 2
};

enum PlaybackState {
  STATE_STOPPED,
  STATE_STARTING,
  STATE_BUFFERING,
  STATE_PLAYING,
  STATE_PAUSED,
  STATE_ERROR
};

enum NetworkState {
  NET_OFF,
  NET_AP_ONLY,
  NET_STA_CONNECTING,
  NET_STA_CONNECTED,
  NET_ERROR
};

struct SourceOpenResult {
  CodecHint   codec;
  std::string title;
  bool        isLive;
};

struct RuntimeStatus {
  PlaybackMode  selectedMode;
  PlaybackState playbackState;
  NetworkState  networkState;
  std::string   nowPlaying;
  std::string   lastError;
};

struct GlobalSettings {
  PlaybackMode selectedMode;
  int volume;
  int bass;
  int treble;
};

// Sound processing for the exciter (see audio_dsp.cpp).
struct SoundSettings {
  bool mono;          // equal-power downmix to both channels
  int  enhancer;      // 0 = off, 1..100 (Warm / Full / Punch)
  int  protectHz;     // exciter high-pass, 0 = off, 40..400
  int  notchHz;       // resonance notch centre, 100..2000
  int  notchDepthDb;  // resonance cut, 0 = off, 1..12
};

struct MqttSettings {
  bool        enabled;
  std::string host;
  int         port;
  std::string user;
  std::string password;
  std::string name;  // device name shown in Home Assistant
};

struct WifiSettings {
  std::string ssid;
  std::string password;
};

// ============================================================
//  CONSTANTS  (defined in main.cpp)
// ============================================================

extern const uint32_t SD_RESCAN_INTERVAL_MS;
extern const size_t   SD_MAX_TRACKS;
extern const size_t   RADIO_BUFFER_SIZE;
extern const size_t   RADIO_PREBUFFER_BYTES;
extern const size_t   AUDIO_CHUNK_SIZE;
extern const int      AUDIO_TASK_STACK_SIZE;
extern const int      AUDIO_TASK_PRIORITY;
extern const int      AUDIO_TASK_CORE;
extern const uint32_t WIFI_CONNECT_TIMEOUT_MS;
extern const int      RADIO_MAX_RECOVERIES;
extern const uint32_t RECONNECT_INTERVAL_MS;
extern const uint32_t STREAM_TIMEOUT_MS;
extern const char*    SD_MOUNT_POINT;
extern const char*    SD_LIBRARY_ROOT;
extern const char*    CONFIG_AP_SSID;
extern const char*    CONFIG_AP_PASSWORD;

// ============================================================
//  GLOBAL STATE  (defined in main.cpp)
// ============================================================

extern std::vector<std::string> sdTracks;
extern int  currentSDTrack;
extern bool sdMounted;

extern std::vector<Station> stations;
extern int currentStation;

extern RuntimeStatus  runtimeStatus;
extern GlobalSettings globalSettings;
extern WifiSettings   wifiSettings;
extern SoundSettings  soundSettings;
extern MqttSettings   mqttSettings;

extern EspAudioDecoder audioDecoder;
extern int audioTaskCore;

// Monotonic milliseconds since boot.
uint32_t millisNow();

// ============================================================
//  FUNCTION DECLARATIONS
// ============================================================

// --- audio_dsp.cpp ---
void dspApplySettings();
void dspProcess(int16_t* stereo, size_t frames, uint32_t sampleRate);

// --- audio_output.cpp ---
bool audioOutputInit();
void audioOutputSetVolume(int volume);
void audioOutputWrite(const int16_t* stereo, size_t frames, uint32_t sampleRate);
void audioOutputSilence();

// --- audio_task.cpp ---
void initAudioTask();
bool audioStateLock(uint32_t timeoutMs);
void audioStateUnlock();
bool startDecodedStream(const SourceOpenResult& result);
void stopDecodedStream();
void playbackSetVolume(int value);
void playbackSetBass(int value);
void playbackSetTreble(int value);
bool playbackSetSound(const std::string& key, int value);
CodecHint codecHintFromPath(const std::string& path);
CodecHint codecHintFromMime(const std::string& mime);
bool mimeIsPlaylist(const std::string& mime);

// --- mode_manager.cpp ---
const char* playbackModeLabel(PlaybackMode mode);
const char* playbackStateLabel(PlaybackState state);
const char* networkStateLabel(NetworkState state);
bool modeRequiresNetwork(PlaybackMode mode);
void modeStop();
bool modeOpenSelection();
bool modeBeginSelected();
bool modeSwitchTo(PlaybackMode mode);
bool playbackSelectMode(PlaybackMode mode);
bool playbackPlay();
void playbackPause();
void playbackStop();
bool playbackSelectItem(int index);
bool playbackRefreshLibrary();
bool modeLoop();

// --- network_manager.cpp ---
void networkInit();
bool networkHasWifiCredentials();
bool networkIsConnected();
bool networkConfigPortalActive();
void networkStopAp();
void networkStartConfigPortal();
bool networkEnsureRemoteAccess();
bool networkEnsureOptionalAccess();
void networkBegin();
std::string networkStaIp();

// --- captive_dns.cpp ---
void captiveDnsStart(uint32_t apIp);
void captiveDnsStop();

// --- settings_store.cpp ---
void settingsInit();
void settingsSaveWifi(const std::string& ssid, const std::string& password);
void settingsClearWifi();
void settingsLoadAll();
void settingsSaveGlobal();
void settingsSaveRadio();
void settingsSaveSd();
void settingsSaveVolume();
void settingsSaveMqtt();
void loadStations();
void saveStations();

// --- source_ftp.cpp ---
bool ftpBegin();
void ftpEnd();
bool ftpLoop();
bool ftpRefreshLibrary();
bool ftpSelectItem(int index);
bool ftpOpenSelected();

// --- source_radio.cpp ---
bool radioBegin();
void radioEnd();
bool radioSelectStation(int index);
bool radioOpenSelected();
bool radioHandleFailure();
bool radioLoop();

// --- source_sd.cpp ---
bool sdRefreshLibrary();
bool sdBegin();
void sdEnd();
bool sdSelectTrack(int index);
bool sdOpenSelected();
bool sdLoop();
bool sdCardInfo(std::string& type, uint64_t& sizeBytes);

// --- remote_control.cpp ---
void remoteInit();
bool remoteCommand(const std::string& cmd);
bool remoteSet(const std::string& key, const std::string& value);
bool remoteRamping();

// --- mqtt_bridge.cpp ---
void mqttStart();
void mqttStop();
const char* mqttStatusText();
std::string mqttBaseTopic();
std::string mqttDeviceSuffix();

// --- log_stream.cpp ---
void logStreamInit();
void logStreamStartTcp();
uint32_t logStreamPosition();
uint32_t logStreamOldest();
size_t logStreamRead(uint32_t& pos, char* out, size_t maxLen);

// --- ota_update.cpp ---
void otaBootCheck();
void otaMarkHealthy();
std::string otaRunningInfo();
bool otaBegin(size_t imageSize, std::string& err);
bool otaWrite(const void* data, size_t len, std::string& err);
bool otaFinish(std::string& err);
void otaAbort();

// --- web_server.cpp ---
bool startWebServer();
