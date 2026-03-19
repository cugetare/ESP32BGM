#include "globals.h"
bool radioRequiresNetwork() {
  return true;
}

bool radioBegin() {
  loadStations();
  if (stations.empty()) {
    runtimeStatus.lastError = "No stations configured";
    return false;
  }

  if (currentStation < 0 || currentStation >= (int)stations.size()) {
    currentStation = 0;
  }

  radioRecoveryCount = 0;
  settingsSaveRadio();
  return true;
}

void radioEnd() {
  if (radioStreamStarted) {
    url.end();
    radioStreamStarted = false;
  }
}

bool radioSelectStation(int index) {
  if (index < 0 || index >= (int)stations.size()) return false;
  currentStation = index;
  radioRecoveryCount = 0;
  settingsSaveRadio();
  return true;
}

bool radioOpenSelected(SourceOpenResult& out) {
  if (stations.empty()) return false;
  if (currentStation < 0 || currentStation >= (int)stations.size()) currentStation = 0;

  initAudioPipeline(false);

  if (radioStreamStarted) {
    url.end();
    radioStreamStarted = false;
  }

  const char* acceptMime = stations[currentStation].mime.length() > 0
    ? stations[currentStation].mime.c_str()
    : nullptr;
  if (!url.begin(stations[currentStation].url.c_str(), acceptMime)) {
    runtimeStatus.lastError = "Failed to open radio stream";
    return false;
  }

  radioStreamStarted = true;

  String resolvedMime = stations[currentStation].mime;
  const char* replyMime = url.mime();
  if (resolvedMime.length() == 0 && replyMime != nullptr) {
    resolvedMime = replyMime;
  }

  String lowerResolvedMime = resolvedMime;
  lowerResolvedMime.toLowerCase();
  CodecHint resolvedCodec = codecHintFromMime(lowerResolvedMime);

  if (resolvedCodec == CODEC_UNKNOWN) {
    runtimeStatus.lastError = "Unsupported radio MIME: " + resolvedMime;
    url.end();
    radioStreamStarted = false;
    return false;
  }

  out.stream = &bufferedUrl;
  out.codec = resolvedCodec;
  out.title = stations[currentStation].name;
  out.isLive = true;
  out.seekable = false;
  out.usesSdPins = false;

  if (!startDecodedStream(out)) {
    url.end();
    radioStreamStarted = false;
    return false;
  }

  settingsSaveRadio();
  return true;
}

bool radioHandleFailure() {
  radioRecoveryCount++;
  lastReconnectAttempt = millis();

  SourceOpenResult out = { nullptr, CODEC_UNKNOWN, "", true, false, false };
  if (radioOpenSelected(out)) {
    return true;
  }

  if (radioRecoveryCount >= RADIO_MAX_RECOVERIES) {
    runtimeStatus.playbackState = STATE_ERROR;
    runtimeStatus.lastError = "Radio recovery limit reached";
  }
  return false;
}

void radioLoop() {
  if (runtimeStatus.playbackState == STATE_PAUSED ||
      runtimeStatus.playbackState == STATE_STOPPED ||
      runtimeStatus.playbackState == STATE_ERROR) {
    return;
  }

  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    runtimeStatus.networkState = NET_ERROR;
    if (now - lastReconnectAttempt > RECONNECT_INTERVAL_MS) {
      radioHandleFailure();
    }
    return;
  }

  runtimeStatus.networkState = NET_STA_CONNECTED;

  size_t copied = copierUrl.copy();
  bool hasAudio = copied > 0;

  if (hasAudio) {
    lastAudioData = now;
    radioRecoveryCount = 0;
    runtimeStatus.playbackState = STATE_PLAYING;
    return;
  }

  runtimeStatus.playbackState = STATE_BUFFERING;
  if ((now - lastAudioData > STREAM_TIMEOUT_MS) &&
      (now - lastReconnectAttempt > RECONNECT_INTERVAL_MS)) {
    radioHandleFailure();
  }
}
