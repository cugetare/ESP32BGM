// ============================================================
//  FTP LIBRARY SOURCE (not implemented yet)
// ============================================================
#include "app_state.h"

bool ftpBegin() {
  runtimeStatus.lastError = "FTP mode not implemented yet";
  runtimeStatus.playbackState = STATE_ERROR;
  return false;
}

void ftpEnd() {
}

bool ftpLoop() {
  return false;
}

bool ftpRefreshLibrary() {
  runtimeStatus.lastError = "FTP refresh not implemented yet";
  return false;
}

bool ftpSelectItem(int index) {
  (void)index;
  runtimeStatus.lastError = "FTP selection not implemented yet";
  return false;
}

bool ftpOpenSelected() {
  runtimeStatus.lastError = "FTP open not implemented yet";
  return false;
}
