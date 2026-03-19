#include "globals.h"
bool isSupportedTrack(const String& name) {
  CodecHint codec = codecHintFromPath(name);
  return codec == CODEC_MP3 || codec == CODEC_AAC;
}

void scanSdDirectory(File directory, uint32_t& entriesScanned) {
  File entry = directory.openNextFile();
  while (entry) {
    entriesScanned++;
    if (entry.isDirectory()) {
      scanSdDirectory(entry, entriesScanned);
    } else {
      String name = String(entry.name());
      if (isSupportedTrack(name)) {
        sdTracks.push_back(name);
      }
    }

    // Cooperative scheduling: avoid starving the idle task on large cards.
    if ((entriesScanned & 0x1F) == 0) {
      vTaskDelay(1);
    }

    entry.close();
    entry = directory.openNextFile();
  }
}

void mountSD() {
  if (sdMounted) return;

  SD_MMC.end();
  delay(20);
  if (SD_MMC.begin("/sdcard", true, true)) {
    uint8_t type = SD_MMC.cardType();
    uint64_t sizeMb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    if (type == CARD_NONE || sizeMb == 0) {
      sdMounted = false;
      SD_MMC.end();
      Serial.println("SD mount failed: no card detected");
      return;
    }

    sdMounted = true;
    const char* typeName = "UNKNOWN";
    if (type == CARD_MMC) typeName = "MMC";
    else if (type == CARD_SD) typeName = "SDSC";
    else if (type == CARD_SDHC) typeName = "SDHC";
    Serial.printf("SD OK: %s  %lluMB\n", typeName,sizeMb);
  } else {
    sdMounted = false;
    Serial.println("SD mount failed");
  }
}

bool sdRequiresNetwork() {
  return false;
}

bool sdRefreshLibrary() {
  mountSD();
  lastSDScanAttempt = millis();
  if (!sdMounted) {
    runtimeStatus.lastError = "SD card not mounted";
    return false;
  }

  sdTracks.clear();

  String scanRoot = SD_LIBRARY_ROOT;
  File root = SD_MMC.open(scanRoot.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    scanRoot = "/";
    root = SD_MMC.open(scanRoot.c_str());
  }

  if (!root || !root.isDirectory()) {
    if (root) root.close();
    sdMounted = false;
    SD_MMC.end();
    runtimeStatus.lastError = "Failed to open SD library root";
    return false;
  }

  uint32_t entriesScanned = 0;
  scanSdDirectory(root, entriesScanned);
  root.close();

  // Sorting very large libraries can stall startup and trigger WDT.
  if (sdTracks.size() <= 512) {
    std::sort(sdTracks.begin(), sdTracks.end(),
              [](const String& a, const String& b) {
                return a.compareTo(b) < 0;
              });
  } else {
    Serial.printf("Skipping SD sort for %d tracks to keep startup responsive\n", (int)sdTracks.size());
  }

  if (sdTracks.empty()) {
    runtimeStatus.lastError = "No audio files found on the SD card";
    return false;
  }

  if (currentSDTrack >= (int)sdTracks.size()) currentSDTrack = 0;
  settingsSaveSd();
  return true;
}

bool sdBegin() {
  initAudioPipeline(true);
  return sdRefreshLibrary();
}

void sdEnd() {
  if (sdAudioFile) sdAudioFile.close();
  if (sdMounted) {
    SD_MMC.end();
    sdMounted = false;
  }
}

bool sdSelectTrack(int index) {
  if (index < 0 || index >= (int)sdTracks.size()) return false;
  currentSDTrack = index;
  settingsSaveSd();
  return true;
}

bool sdOpenSelected(SourceOpenResult& out) {
  if (sdTracks.empty() && !sdRefreshLibrary()) return false;
  if (currentSDTrack >= (int)sdTracks.size()) currentSDTrack = 0;

  if (sdAudioFile) sdAudioFile.close();

  String path = sdTracks[currentSDTrack];
  if (!path.startsWith("/")) path = "/" + path;
  sdAudioFile = SD_MMC.open(path.c_str());
  if (!sdAudioFile) {
    sdMounted = false;
    SD_MMC.end();
    runtimeStatus.lastError = "Failed to open " + path;
    return false;
  }

  out.stream = &sdAudioFile;
  out.codec = codecHintFromPath(path);
  out.title = sdTracks[currentSDTrack];
  out.isLive = false;
  out.seekable = true;
  out.usesSdPins = true;

  if (!startDecodedStream(out)) {
    sdAudioFile.close();
    return false;
  }

  settingsSaveSd();
  return true;
}

void sdLoop() {
  if (runtimeStatus.playbackState == STATE_PAUSED ||
      runtimeStatus.playbackState == STATE_STOPPED ||
      runtimeStatus.playbackState == STATE_ERROR) {
    return;
  }

  unsigned long now = millis();
  if ((!sdMounted || sdTracks.empty() || !sdAudioFile) &&
      (now - lastSDScanAttempt > SD_RESCAN_INTERVAL_MS)) {
    if (sdRefreshLibrary()) {
      SourceOpenResult out = { nullptr, CODEC_UNKNOWN, "", false, false, true };
      sdOpenSelected(out);
    }
    return;
  }

  if (sdAudioFile && sdAudioFile.available()) {
    copierSD.copy();
    runtimeStatus.playbackState = STATE_PLAYING;
    lastAudioData = now;
    return;
  }

  if (!sdTracks.empty()) {
    currentSDTrack++;
    if (currentSDTrack >= (int)sdTracks.size()) currentSDTrack = 0;
    SourceOpenResult out = { nullptr, CODEC_UNKNOWN, "", false, false, true };
    sdOpenSelected(out);
  }
}
