// ============================================================
//  SD CARD SOURCE  (SDMMC 1-bit: CLK 14, CMD 15, D0 2)
// ============================================================
#include "app_state.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"

static const char* TAG = "sd";

static sdmmc_card_t* s_card = nullptr;
static FILE* s_file = nullptr;
static char* s_fileBuf = nullptr;
static uint8_t* s_chunk = nullptr;
static uint32_t s_lastScanAttempt = 0;

static const size_t FILE_BUFFER_SIZE = 16 * 1024;

static void closeTrackFile() {
  if (s_file != nullptr) {
    fclose(s_file);
    s_file = nullptr;
  }
}

static void mountSD() {
  if (sdMounted) return;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;  // 1-bit keeps GPIO 4/12/13 free for keys and codec straps
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_sdmmc_mount_config_t mountCfg = {};
  mountCfg.format_if_mount_failed = false;
  mountCfg.max_files = 4;
  mountCfg.allocation_unit_size = 16 * 1024;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mountCfg, &s_card);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "SD mount failed (%s)", esp_err_to_name(ret));
    s_card = nullptr;
    sdMounted = false;
    return;
  }

  sdMounted = true;
  sdmmc_card_print_info(stdout, s_card);
}

static void unmountSD() {
  closeTrackFile();
  if (sdMounted) {
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    sdMounted = false;
  }
}

// relPath is relative to the card root ("/music/a.mp3").
static void scanSdDirectory(const std::string& relPath, uint32_t& entriesScanned) {
  std::string full = std::string(SD_MOUNT_POINT) + relPath;
  DIR* dir = opendir(full.c_str());
  if (dir == nullptr) return;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr && sdTracks.size() < SD_MAX_TRACKS) {
    entriesScanned++;
    if (entry->d_name[0] == '.') continue;  // ".", "..", hidden files

    std::string child = relPath == "/" ? "/" + std::string(entry->d_name)
                                       : relPath + "/" + entry->d_name;
    if (entry->d_type == DT_DIR) {
      scanSdDirectory(child, entriesScanned);
    } else if (codecHintFromPath(child) != CODEC_UNKNOWN) {
      sdTracks.push_back(child);
    }

    // Cooperative scheduling: avoid starving the idle task on large cards.
    if ((entriesScanned & 0x1F) == 0) vTaskDelay(1);
  }
  closedir(dir);
}

bool sdRefreshLibrary() {
  mountSD();
  s_lastScanAttempt = millisNow();
  if (!sdMounted) {
    runtimeStatus.lastError = "SD card not mounted";
    return false;
  }

  closeTrackFile();
  sdTracks.clear();

  std::string scanRoot = SD_LIBRARY_ROOT;
  struct stat st = {};
  std::string fullRoot = std::string(SD_MOUNT_POINT) + scanRoot;
  if (stat(fullRoot.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    scanRoot = "/";
  }

  uint32_t entriesScanned = 0;
  scanSdDirectory(scanRoot, entriesScanned);

  if (sdTracks.size() >= SD_MAX_TRACKS) {
    ESP_LOGW(TAG, "track limit (%u) reached, remaining files ignored", (unsigned)SD_MAX_TRACKS);
  }
  std::sort(sdTracks.begin(), sdTracks.end());
  ESP_LOGI(TAG, "%u tracks found under %s", (unsigned)sdTracks.size(), scanRoot.c_str());

  if (sdTracks.empty()) {
    runtimeStatus.lastError = "No audio files found on the SD card";
    return false;
  }

  if (currentSDTrack < 0 || currentSDTrack >= (int)sdTracks.size()) currentSDTrack = 0;
  settingsSaveSd();
  return true;
}

bool sdBegin() {
  if (s_fileBuf == nullptr) {
    s_fileBuf = (char*)heap_caps_malloc(FILE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (s_chunk == nullptr) {
    s_chunk = (uint8_t*)heap_caps_malloc(AUDIO_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (s_chunk == nullptr) {
    runtimeStatus.lastError = "Out of memory";
    return false;
  }
  return sdRefreshLibrary();
}

void sdEnd() {
  unmountSD();
}

bool sdSelectTrack(int index) {
  if (index < 0 || index >= (int)sdTracks.size()) return false;
  currentSDTrack = index;
  settingsSaveSd();
  return true;
}

bool sdOpenSelected() {
  if (sdTracks.empty() && !sdRefreshLibrary()) return false;
  if (currentSDTrack < 0 || currentSDTrack >= (int)sdTracks.size()) currentSDTrack = 0;

  closeTrackFile();

  const std::string& path = sdTracks[currentSDTrack];
  std::string full = std::string(SD_MOUNT_POINT) + path;
  s_file = fopen(full.c_str(), "rb");
  if (s_file == nullptr) {
    // The card may have been removed: remount on the next loop.
    unmountSD();
    runtimeStatus.lastError = "Failed to open " + path;
    return false;
  }
  if (s_fileBuf != nullptr) setvbuf(s_file, s_fileBuf, _IOFBF, FILE_BUFFER_SIZE);

  SourceOpenResult out;
  out.codec = codecHintFromPath(path);
  out.title = path;
  out.isLive = false;

  if (!startDecodedStream(out)) {
    closeTrackFile();
    return false;
  }

  settingsSaveSd();
  return true;
}

bool sdLoop() {
  if (runtimeStatus.playbackState == STATE_PAUSED ||
      runtimeStatus.playbackState == STATE_STOPPED ||
      runtimeStatus.playbackState == STATE_ERROR) {
    return false;
  }

  uint32_t now = millisNow();
  if (!sdMounted || sdTracks.empty() || s_file == nullptr) {
    if (now - s_lastScanAttempt > SD_RESCAN_INTERVAL_MS && sdRefreshLibrary()) {
      sdOpenSelected();
    }
    return false;
  }

  size_t n = fread(s_chunk, 1, AUDIO_CHUNK_SIZE, s_file);
  if (n > 0) {
    audioDecoder.write(s_chunk, n);
    runtimeStatus.playbackState = STATE_PLAYING;
    return true;
  }

  if (ferror(s_file)) {
    ESP_LOGW(TAG, "read error on %s", sdTracks[currentSDTrack].c_str());
  }

  // End of track: flush parser caches (FLAC) and advance.
  audioDecoder.finish();
  closeTrackFile();
  currentSDTrack = (currentSDTrack + 1) % (int)sdTracks.size();
  sdOpenSelected();
  return true;
}

bool sdCardInfo(std::string& type, uint64_t& sizeBytes) {
  if (!sdMounted || s_card == nullptr) return false;
  if (s_card->is_mmc) type = "MMC";
  else if (s_card->ocr & SD_OCR_SDHC_CAP) type = "SDHC/SDXC";
  else type = "SDSC";
  sizeBytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
  return true;
}
