#include "globals.h"
void initAudioTask() {
  if (audioTaskHandle != nullptr) return;

  BaseType_t currentCore = xPortGetCoreID();
  audioTaskCore = currentCore;
  xTaskCreatePinnedToCore(
    audioTaskRunner,
    "audioTask",
    AUDIO_TASK_STACK_SIZE,
    nullptr,
    AUDIO_TASK_PRIORITY,
    &audioTaskHandle,
    audioTaskCore
  );
}

void audioTaskRunner(void* parameter) {
  (void)parameter;
  for (;;) {
    if (audioTaskHold) {
      audioTaskIdle = true;
      vTaskDelay(1);
      continue;
    }

    audioTaskIdle = false;
    modeLoop();
    audioTaskIdle = true;
    vTaskDelay(1);
  }
}

bool audioStateLock(uint32_t timeoutMs) {
  if (audioStateMutex == nullptr) return true;
  TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  bool pausedAudioTask = false;

  if (audioTaskHandle != nullptr && currentTask != audioTaskHandle) {
    audioTaskHold = true;
    pausedAudioTask = true;

    unsigned long start = millis();
    while (!audioTaskIdle) {
      if (millis() - start >= timeoutMs) {
        audioTaskHold = false;
        return false;
      }
      vTaskDelay(1);
    }
  }

  bool locked = xSemaphoreTake(audioStateMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  if (!locked && pausedAudioTask) {
    audioTaskHold = false;
  }
  return locked;
}

void audioStateUnlock() {
  if (audioStateMutex != nullptr) {
    xSemaphoreGive(audioStateMutex);
  }
  if (audioTaskHandle != nullptr && xTaskGetCurrentTaskHandle() != audioTaskHandle) {
    audioTaskHold = false;
  }
}
