#include "globals.h"
bool ftpRequiresNetwork() {
  return true;
}

bool ftpBegin() {
  runtimeStatus.lastError = "FTP mode not implemented yet";
  runtimeStatus.playbackState = STATE_ERROR;
  return false;
}

void ftpEnd() {
}

void ftpLoop() {
}

bool ftpRefreshLibrary() {
  runtimeStatus.lastError = "FTP refresh not implemented yet";
  return false;
}

bool ftpSelectItem(int index) {
  ftpSettings.currentIndex = index;
  runtimeStatus.lastError = "FTP selection not implemented yet";
  return false;
}

bool ftpOpenSelected(SourceOpenResult& out) {
  out.stream = nullptr;
  out.codec = CODEC_UNKNOWN;
  out.title = "";
  out.isLive = false;
  out.seekable = false;
  out.usesSdPins = false;
  runtimeStatus.lastError = "FTP open not implemented yet";
  return false;
}
