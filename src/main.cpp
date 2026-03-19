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
// Initialize hardware components such as I2S, audio codec, and any other peripherals. This sets up the necessary interfaces for audio playback and other hardware interactions.  
  initHardware();
// Load all settings from non-volatile storage (Preferences). This includes WiFi credentials, station list, and other configuration parameters. This ensures that the device can restore its previous state after a reboot.
  settingsLoadAll();
//The mutex (mutual exclusion lock) ensures only one task modifies audio state at a time.
  audioStateMutex = xSemaphoreCreateMutex();
// Create the audio task that will run the audio processing loop. This allows audio playback to run concurrently with the main loop, which handles network and user interactions.
//  registerRoutes(); // Moved to WebTask to ensure it starts after audio initialization, preventing potential conflicts during startup.
// Initialize WiFi and network services. This includes connecting to WiFi, starting the web server, and any other network-related setup.
  networkBegin();
// Start the DNS server to handle domain name resolution for the captive portal when in configuration mode.
//  server.begin(); // Moved to WebTask to ensure it starts after audio initialization, preventing potential conflicts during startup.
// If the device is in configuration mode (e.g., acting as a WiFi access point for setup), start the DNS server to redirect all DNS queries to the device's IP address, enabling the captive portal functionality.
  modeBeginSelected();
// Initialize the audio task, which will handle audio playback in a separate thread. This allows the main loop to remain responsive for network and user interactions while audio is playing.
  initAudioTask();
// Start the web server task, which will handle incoming HTTP requests in a separate thread. This allows the main loop to focus on network management and other tasks without being blocked by web server processing.
  startWebTask();
}

// ============================================================
//  LOOP
// ============================================================

void loop() {
  networkLoop();
  mqttLoop();
  //  server.handleClient(); // Moved to WebTask to ensure it starts after audio initialization, preventing potential conflicts during startup.
  //  yield();
  vTaskDelay(1);
}
