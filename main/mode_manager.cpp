// ============================================================
//  MODE MANAGER
//  Callers from other tasks take the audio state lock (recursive),
//  so the audio task never sees a half-switched source.
// ============================================================
#include "app_state.h"

const char* playbackModeLabel(PlaybackMode mode) {
  switch (mode) {
    case PLAYBACK_RADIO_URL: return "Radio URL";
    case PLAYBACK_SD_CARD: return "SD Card";
    case PLAYBACK_FTP_LIBRARY: return "FTP Library";
  }
  return "Unknown";
}

const char* playbackStateLabel(PlaybackState state) {
  switch (state) {
    case STATE_STOPPED: return "Stopped";
    case STATE_STARTING: return "Starting";
    case STATE_BUFFERING: return "Buffering";
    case STATE_PLAYING: return "Playing";
    case STATE_PAUSED: return "Paused";
    case STATE_ERROR: return "Error";
  }
  return "Unknown";
}

const char* networkStateLabel(NetworkState state) {
  switch (state) {
    case NET_OFF: return "Off";
    case NET_AP_ONLY: return "AP";
    case NET_STA_CONNECTING: return "Connecting";
    case NET_STA_CONNECTED: return "Connected";
    case NET_ERROR: return "Error";
  }
  return "Unknown";
}

bool modeRequiresNetwork(PlaybackMode mode) {
  return mode == PLAYBACK_RADIO_URL || mode == PLAYBACK_FTP_LIBRARY;
}

void modeStop() {
  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_RADIO_URL:
      radioEnd();
      break;
    case PLAYBACK_SD_CARD:
      sdEnd();
      break;
    case PLAYBACK_FTP_LIBRARY:
      ftpEnd();
      break;
  }
  stopDecodedStream();
}

bool modeOpenSelection() {
  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_RADIO_URL:
      return radioOpenSelected();
    case PLAYBACK_SD_CARD:
      return sdOpenSelected();
    case PLAYBACK_FTP_LIBRARY:
      return ftpOpenSelected();
  }
  return false;
}

bool modeBeginSelected() {
  return modeSwitchTo(globalSettings.selectedMode);
}

bool modeSwitchTo(PlaybackMode mode) {
  if (!audioStateLock(5000)) {
    runtimeStatus.lastError = "Audio engine busy";
    return false;
  }

  modeStop();

  if (modeRequiresNetwork(mode)) {
    loadStations();
  }

  globalSettings.selectedMode = mode;
  runtimeStatus.selectedMode = mode;
  runtimeStatus.playbackState = STATE_STARTING;
  runtimeStatus.lastError.clear();
  settingsSaveGlobal();

  if (modeRequiresNetwork(mode)) {
    if (!networkEnsureRemoteAccess()) {
      runtimeStatus.playbackState = STATE_ERROR;
      audioStateUnlock();
      return false;
    }
  } else {
    networkEnsureOptionalAccess();
  }

  bool started = false;
  switch (mode) {
    case PLAYBACK_RADIO_URL:
      started = radioBegin();
      break;
    case PLAYBACK_SD_CARD:
      started = sdBegin();
      break;
    case PLAYBACK_FTP_LIBRARY:
      started = ftpBegin();
      break;
  }
  if (!started) {
    runtimeStatus.playbackState = STATE_ERROR;
    audioStateUnlock();
    return false;
  }

  if (modeOpenSelection()) {
    audioStateUnlock();
    return true;
  }

  // The remembered SD track may be gone or unreadable: try the others.
  if (mode == PLAYBACK_SD_CARD && !sdTracks.empty()) {
    int failed = currentSDTrack;
    for (size_t i = 0; i < sdTracks.size(); ++i) {
      if ((int)i == failed) continue;
      currentSDTrack = (int)i;
      if (modeOpenSelection()) {
        settingsSaveSd();
        audioStateUnlock();
        return true;
      }
    }
  }

  runtimeStatus.playbackState = STATE_ERROR;
  audioStateUnlock();
  return false;
}

bool playbackSelectMode(PlaybackMode mode) {
  return modeSwitchTo(mode);
}

bool playbackPlay() {
  if (!audioStateLock(1000)) {
    runtimeStatus.lastError = "Audio engine busy";
    return false;
  }

  if (runtimeStatus.playbackState == STATE_PLAYING) {
    audioStateUnlock();
    return true;
  }

  if (runtimeStatus.playbackState == STATE_STOPPED ||
      runtimeStatus.playbackState == STATE_ERROR ||
      runtimeStatus.selectedMode == PLAYBACK_RADIO_URL) {
    bool opened = modeOpenSelection();
    audioStateUnlock();
    return opened;
  }

  runtimeStatus.playbackState = STATE_PLAYING;
  audioStateUnlock();
  return true;
}

void playbackPause() {
  if (!audioStateLock(1000)) return;
  if (runtimeStatus.playbackState != STATE_PLAYING &&
      runtimeStatus.playbackState != STATE_BUFFERING) {
    audioStateUnlock();
    return;
  }

  // A live stream cannot be paused; drop it and reconnect on play.
  if (runtimeStatus.selectedMode == PLAYBACK_RADIO_URL) {
    radioEnd();
    audioDecoder.end();
  }
  audioOutputSilence();
  runtimeStatus.playbackState = STATE_PAUSED;
  audioStateUnlock();
}

void playbackStop() {
  if (!audioStateLock(1000)) return;
  modeStop();
  audioStateUnlock();
}

bool playbackSelectItem(int index) {
  if (!audioStateLock(2000)) {
    runtimeStatus.lastError = "Audio engine busy";
    return false;
  }

  bool ok = false;
  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_RADIO_URL:
      ok = radioSelectStation(index);
      break;
    case PLAYBACK_SD_CARD:
      ok = sdSelectTrack(index);
      break;
    case PLAYBACK_FTP_LIBRARY:
      ok = ftpSelectItem(index);
      break;
  }

  bool opened = ok && modeOpenSelection();
  audioStateUnlock();
  return opened;
}

bool playbackRefreshLibrary() {
  if (!audioStateLock(3000)) {
    runtimeStatus.lastError = "Audio engine busy";
    return false;
  }

  bool ok = false;
  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_SD_CARD:
      ok = sdRefreshLibrary() && modeOpenSelection();
      break;
    case PLAYBACK_FTP_LIBRARY:
      ok = ftpRefreshLibrary();
      break;
    case PLAYBACK_RADIO_URL:
      ok = radioBegin() && modeOpenSelection();
      break;
  }
  audioStateUnlock();
  return ok;
}

// Called by the audio task with the state lock held.
// Returns true when it moved audio data this iteration.
bool modeLoop() {
  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_RADIO_URL:
      return radioLoop();
    case PLAYBACK_SD_CARD:
      return sdLoop();
    case PLAYBACK_FTP_LIBRARY:
      return ftpLoop();
  }
  return false;
}
