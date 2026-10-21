#include "globals.h"
String playbackModeLabel(PlaybackMode mode) {
  switch (mode) {
    case PLAYBACK_RADIO_URL: return "Radio URL";
    case PLAYBACK_SD_CARD: return "SD Card";
    case PLAYBACK_FTP_LIBRARY: return "FTP Library";
  }
  return "Unknown";
}

String playbackStateLabel(PlaybackState state) {
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

String networkStateLabel(NetworkState state) {
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

bool modeUsesSdPins(PlaybackMode mode) {
  return mode == PLAYBACK_SD_CARD;
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
  SourceOpenResult out = { nullptr, CODEC_UNKNOWN, "", false, false, false };

  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_RADIO_URL:
      return radioOpenSelected(out);
    case PLAYBACK_SD_CARD:
      return sdOpenSelected(out);
    case PLAYBACK_FTP_LIBRARY:
      return ftpOpenSelected(out);
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

  // Load stations when switching to network-based modes
  if (modeRequiresNetwork(mode)) {
    loadStations();
  }

  globalSettings.selectedMode = mode;
  runtimeStatus.selectedMode = mode;
  runtimeStatus.playbackState = STATE_STARTING;
  runtimeStatus.lastError = "";
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
      if (!started) {
        runtimeStatus.playbackState = STATE_ERROR;
        audioStateUnlock();
        return false;
      }
      if (!modeOpenSelection()) {
        if (runtimeStatus.playbackState != STATE_ERROR) {
          audioStateUnlock();
          return false;
        }
        audioStateUnlock();
        return false;
      }
      break;
    case PLAYBACK_SD_CARD:
      started = sdBegin();
      if (!started) {
        runtimeStatus.playbackState = STATE_ERROR;
        audioStateUnlock();
        return false;
      }
      if (!modeOpenSelection()) {
        runtimeStatus.playbackState = STATE_ERROR;
        audioStateUnlock();
        return false;
      }
      break;
    case PLAYBACK_FTP_LIBRARY:
      started = ftpBegin();
      if (!started) {
        runtimeStatus.playbackState = STATE_ERROR;
        audioStateUnlock();
        return false;
      }
      if (!modeOpenSelection()) {
        runtimeStatus.playbackState = STATE_ERROR;
        audioStateUnlock();
        return false;
      }
      break;
  }

  if (mode == PLAYBACK_SD_CARD && !sdTracks.empty()) {
    for (size_t i = 0; i < sdTracks.size(); ++i) {
      if ((int)i == currentSDTrack) continue;
      currentSDTrack = i;
      if (modeOpenSelection()) {
        settingsSaveSd();
        audioStateUnlock();
        return true;
      }
    }

    runtimeStatus.playbackState = STATE_ERROR;
    audioStateUnlock();
    return true;
  }

  audioStateUnlock();
  return true;
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

  if (runtimeStatus.selectedMode == PLAYBACK_RADIO_URL) {
    radioEnd();
  }
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

  if (!ok) {
    audioStateUnlock();
    return false;
  }
  bool opened = modeOpenSelection();
  audioStateUnlock();
  return opened;
}

bool playbackRefreshLibrary() {
  if (!audioStateLock(3000)) {
    runtimeStatus.lastError = "Audio engine busy";
    return false;
  }

  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_SD_CARD:
      if (!sdRefreshLibrary()) {
        audioStateUnlock();
        return false;
      }
      {
        bool opened = modeOpenSelection();
        audioStateUnlock();
        return opened;
      }
      break;
    case PLAYBACK_FTP_LIBRARY:
      {
        bool refreshed = ftpRefreshLibrary();
        audioStateUnlock();
        return refreshed;
      }
      break;
    case PLAYBACK_RADIO_URL:
      if (!radioBegin()) {
        audioStateUnlock();
        return false;
      }
      {
        bool opened = modeOpenSelection();
        audioStateUnlock();
        return opened;
      }
      break;
    default:
      audioStateUnlock();
      return false;
  }
}

void modeLoop() {
  switch (runtimeStatus.selectedMode) {
    case PLAYBACK_RADIO_URL:
      radioLoop();
      break;
    case PLAYBACK_SD_CARD:
      sdLoop();
      break;
    case PLAYBACK_FTP_LIBRARY:
      ftpLoop();
      break;
  }
}
