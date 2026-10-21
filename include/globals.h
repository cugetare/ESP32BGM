#pragma once

// ============================================================
//  INCLUDES (shared across project)
// ============================================================

#include <Arduino.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecAACHelix.h"
#include "AudioTools/AudioCodecs/MultiDecoder.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/Communication/AudioHttp.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <SD_MMC.h>

#include <vector>
#include <algorithm>

// ============================================================
//  TYPES
// ============================================================

struct Station {
  String name;
  String url;
  String mime;
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

enum CodecHint {
  CODEC_UNKNOWN,
  CODEC_MP3,
  CODEC_AAC
};

struct MediaItem {
  String name;
  String path;
  CodecHint codec;
  bool isDirectory;
};

struct SourceOpenResult {
  Stream*     stream;
  CodecHint   codec;
  String      title;
  bool        isLive;
  bool        seekable;
  bool        usesSdPins;
};

struct RuntimeStatus {
  PlaybackMode  selectedMode;
  PlaybackState playbackState;
  NetworkState  networkState;
  String        nowPlaying;
  String        lastError;
};

struct GlobalSettings {
  PlaybackMode selectedMode;
  int volume;
  int bass;
  int treble;
};

struct WifiSettings {
  String ssid;
  String password;
};

struct FtpSettings {
  String   host;
  uint16_t port;
  String   user;
  String   password;
  String   basePath;
  int      currentIndex;
};

struct MqttSettings {
  bool     enabled;
  String   host;
  uint16_t port;
  String   user;
  String   password;
  String   baseTopic;
};

// ============================================================
//  GLOBAL VARIABLE DECLARATIONS  (defined once in main.cpp)
// ============================================================

// --- SD ---
extern std::vector<String> sdTracks;
extern int currentSDTrack;
extern bool sdMounted;
extern unsigned long lastSDScanAttempt;
extern File sdAudioFile;

// --- Stations ---
extern std::vector<Station> stations;
extern int currentStation;

// --- Webserver ---
extern volatile bool webserverBusy;

// --- Runtime ---
extern RuntimeStatus runtimeStatus;

// --- Web ---
extern WebServer server;

// --- Audio timing ---
extern unsigned long lastAudioData;

// --- Constants ---
extern const unsigned long SD_RESCAN_INTERVAL_MS;
extern const int URL_BUFFER_SIZE;
extern const int URL_BUFFER_FALLBACK_SIZE;
extern const int AUDIO_TASK_STACK_SIZE;
extern const int AUDIO_TASK_PRIORITY;
extern const int WIFI_TIMEOUT_ATTEMPTS;
extern const int RADIO_MAX_RECOVERIES;
extern const int RECONNECT_INTERVAL_MS;
extern const int STREAM_TIMEOUT_MS;
extern const char* SD_LIBRARY_ROOT;
extern const char* STATIONS_FILE;
extern const char* CONFIG_AP_SSID;
extern const char* CONFIG_AP_PASSWORD;
extern const int DNS_PORT;

// --- Audio objects ---
extern AudioBoardStream i2s;
extern Equalizer3Bands eq;
extern ConfigEqualizer3Bands cfg_eq;
extern MP3DecoderHelix mp3;
extern AACDecoderHelix aac;
extern MultiDecoder multiDecoder;
extern URLStream url;
extern BufferedStream bufferedUrl;
extern EncodedAudioStream dec;
extern StreamCopy copierUrl;
extern StreamCopy copierSD;

// --- State ---
extern CodecHint activeCodec;
extern GlobalSettings globalSettings;
extern WifiSettings wifiSettings;
extern FtpSettings ftpSettings;
extern MqttSettings mqttSettings;

// --- Network ---
extern Preferences prefs;
extern DNSServer dnsServer;
extern bool configApActive;

// --- Radio ---
extern bool radioStreamStarted;
extern unsigned long lastReconnectAttempt;
extern int radioRecoveryCount;

// --- FreeRTOS ---
extern SemaphoreHandle_t audioStateMutex;
extern TaskHandle_t audioTaskHandle;
extern BaseType_t audioTaskCore;
extern volatile bool audioTaskHold;
extern volatile bool audioTaskIdle;

// ============================================================
//  FUNCTION DECLARATIONS
//  Every cross-file call must be declared here.
//  Arduino IDE generated these implicitly; PlatformIO does not.
// ============================================================

// --- AudioPipeline.cpp ---
void initHardware();
void initAudioPipeline(bool sdActive);
void applyEQ();
bool startDecodedStream(const SourceOpenResult& result);
void stopDecodedStream();
void playbackSetVolume(int value);
void playbackSetBass(int value);
void playbackSetTreble(int value);
CodecHint codecHintFromPath(const String& path);
CodecHint codecHintFromMime(const String& mime);
const char* mimeForCodecHint(CodecHint codec);
float readChipTemperatureC();
String formatChipTemperature();

// --- AudioTask.cpp ---
void initAudioTask();
void audioTaskRunner(void* parameter);
bool audioStateLock(uint32_t timeoutMs);
void audioStateUnlock();

// --- ModeManager.cpp ---
String playbackModeLabel(PlaybackMode mode);
String playbackStateLabel(PlaybackState state);
String networkStateLabel(NetworkState state);
bool modeRequiresNetwork(PlaybackMode mode);
bool modeUsesSdPins(PlaybackMode mode);
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
void modeLoop();

// --- MqttBridge.cpp ---
void mqttBegin();
void mqttLoop();
void mqttPublishState();
void mqttPublishNowPlaying();

// --- NetworkManager.cpp ---
bool networkHasWifiCredentials();
void networkStopAp();
void networkStartConfigPortal();
bool networkEnsureRemoteAccess();
bool networkEnsureOptionalAccess();
void networkBegin();
void networkLoop();

// --- SettingsStore.cpp ---
void settingsLoadWifi();
void settingsSaveWifi(const String& ssid, const String& password);
void settingsClearWifi();
void settingsLoadAll();
void settingsSaveGlobal();
void settingsSaveRadio();
void settingsSaveSd();
void loadStations();
void saveStations();

// --- SourceFtpLibrary.cpp ---
bool ftpRequiresNetwork();
bool ftpBegin();
void ftpEnd();
void ftpLoop();
bool ftpRefreshLibrary();
bool ftpSelectItem(int index);
bool ftpOpenSelected(SourceOpenResult& out);

// --- SourceRadioUrl.cpp ---
bool radioRequiresNetwork();
bool radioBegin();
void radioEnd();
bool radioSelectStation(int index);
bool radioOpenSelected(SourceOpenResult& out);
bool radioHandleFailure();
void radioLoop();

// --- SourceSdCard.cpp ---
bool isSupportedTrack(const String& name);
void scanSdDirectory(File directory, uint32_t& entriesScanned);
void mountSD();
bool sdRequiresNetwork();
bool sdRefreshLibrary();
bool sdBegin();
void sdEnd();
bool sdSelectTrack(int index);
bool sdOpenSelected(SourceOpenResult& out);
void sdLoop();

// --- WebHandlers.cpp ---
void registerRoutes();

// --- WebTask.cpp ---
void startWebTask();
void webserverLoop();  // Handles webserverBusy flag and network management
// Individual handlers (called by server.on lambdas internally; declared for clarity)
void handleRoot();
void handleWifiSetup();
void handleSaveWifi();
void handleResetWifi();
void handleReboot();
void handleSetMode();
void handlePlay();
void handlePause();
void handleRefreshLibrary();
void handleSelectItem();
void handleSetVolume();
void handleSetBass();
void handleSetTreble();
void handleSaveDefaults();
void handleManage();
void handleGetStations();
void handleAddStation();
void handleDeleteStation();
void handleSDInfo();
void handleDiagnostics();
