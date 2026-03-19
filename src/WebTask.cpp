/*
 * WebTask.cpp - Web server task for handling HTTP requests on ESP32BGM.
 *
 * This file defines the webTask function, which initializes the web server and handles incoming HTTP requests in a loop. The task is designed to run on core 0 of the ESP32, allowing it to operate concurrently with other tasks (like audio processing) without interference.
 * The web server is set up to start after a short delay to ensure that other critical systems (like audio) have time to initialize properly. This helps prevent potential conflicts during startup and ensures a smoother user experience.
 */


#include "globals.h"

static void webTask(void* param) {
  // Delay to allow other tasks (like audio) to initialize first. This helps ensure that the web server starts after the audio system is up and running, preventing potential conflicts or resource contention during startup.
  vTaskDelay(pdMS_TO_TICKS(3000));

  registerRoutes();
  server.begin();

  for (;;) {
    server.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void startWebTask() {
  xTaskCreatePinnedToCore(
    webTask,
    "webTask",
    8192,
    nullptr,
    1,          // LOW priority
    nullptr,
    0           // core 0 (audio e pe core-ul curent)
  );
}