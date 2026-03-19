#include "globals.h"

// ============================================================
//  CONSTANTS  (declared extern in globals.h, defined here)
// ============================================================

const unsigned long SD_RESCAN_INTERVAL_MS  = 5000;
const int URL_BUFFER_SIZE                  = 1024 * 64;   // 64 KB
const int URL_BUFFER_FALLBACK_SIZE         = 1024 * 16;   // 16 KB
const int AUDIO_TASK_STACK_SIZE            = 1024 * 8;
const int AUDIO_TASK_PRIORITY              = 2;
const int WIFI_TIMEOUT_ATTEMPTS            = 20;
const int RADIO_MAX_RECOVERIES             = 5;
const int RECONNECT_INTERVAL_MS            = 5000;
const int STREAM_TIMEOUT_MS                = 10000;
const char* SD_LIBRARY_ROOT               = "/music";
const char* STATIONS_FILE                 = "/stations.json";
const char* CONFIG_AP_SSID                = "ESP32-Radio-Setup";
const char* CONFIG_AP_PASSWORD            = "radiosetup";
const int DNS_PORT                         = 53;

// ============================================================
//  GLOBAL DEFINITIONS  (one definition per variable)
// ============================================================

// --- SD ---
std::vector<String> sdTracks;
int currentSDTrack        = 0;
bool sdMounted            = false;
unsigned long lastSDScanAttempt = 0;
File sdAudioFile;

// --- Stations ---
std::vector<Station> stations;
int currentStation = 0;

// --- Runtime ---
RuntimeStatus runtimeStatus = {
  PLAYBACK_RADIO_URL,
  STATE_STOPPED,
  NET_OFF,
  "",
  ""
};

// --- Web server ---
WebServer server(80);

// --- Audio timing ---
unsigned long lastAudioData = 0;

// --- Settings ---
GlobalSettings globalSettings = { PLAYBACK_RADIO_URL, 50, 0, 0 };
WifiSettings   wifiSettings   = { "", "" };
FtpSettings    ftpSettings    = { "", 21, "", "", "/", 0 };
MqttSettings   mqttSettings   = { false, "", 1883, "", "", "advanced_webradio" };

// --- Network ---
bool configApActive = false;

// --- Radio ---
bool radioStreamStarted     = false;
unsigned long lastReconnectAttempt = 0;
int radioRecoveryCount      = 0;
CodecHint activeCodec       = CODEC_UNKNOWN;

// --- FreeRTOS ---
SemaphoreHandle_t audioStateMutex = nullptr;
TaskHandle_t      audioTaskHandle  = nullptr;
BaseType_t        audioTaskCore    = 0;
volatile bool audioTaskHold = false;
volatile bool audioTaskIdle = true;

// ============================================================
//  AUDIO OBJECTS
// ============================================================

AudioBoardStream i2s(AudioKitEs8388V1);
Equalizer3Bands  eq(i2s);
ConfigEqualizer3Bands cfg_eq;

MP3DecoderHelix mp3;
AACDecoderHelix aac;
MultiDecoder    multiDecoder;

URLStream      url;
BufferedStream bufferedUrl(0);
EncodedAudioStream dec(&eq, &multiDecoder);

StreamCopy copierUrl(dec, bufferedUrl);
StreamCopy copierSD(dec, sdAudioFile);

// ============================================================
//  NETWORK OBJECTS
// ============================================================

Preferences prefs;
DNSServer   dnsServer;

// ============================================================
//  SETUP
// ============================================================

void setup() {
  initHardware();

  settingsLoadAll();

  audioStateMutex = xSemaphoreCreateMutex();

  registerRoutes();

  networkBegin();
  server.begin();

  modeBeginSelected();

  initAudioTask();
}

// ============================================================
//  LOOP
// ============================================================

void loop() {
  networkLoop();
  mqttLoop();
  server.handleClient();
  yield();
}
