// ============================================================
//  OTA UPDATE
//  Firmware is uploaded from the web UI into the inactive slot.
//  With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a new image boots in
//  PENDING_VERIFY state; it must reach otaMarkHealthy() (web server
//  up) within OTA_VERIFY_TIMEOUT_US or it is rolled back.
// ============================================================
#include "app_state.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char* TAG = "ota";
static const int64_t OTA_VERIFY_TIMEOUT_US = 120LL * 1000 * 1000;

static esp_ota_handle_t s_handle = 0;
static const esp_partition_t* s_target = nullptr;
static size_t s_written = 0;
static esp_timer_handle_t s_verifyTimer = nullptr;
static bool s_pendingVerify = false;

static void verifyTimeout(void* arg) {
  (void)arg;
  ESP_LOGE(TAG, "new firmware never became healthy, rolling back");
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

static void restartLater(void* arg) {
  (void)arg;
  esp_restart();
}

void otaBootCheck() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_app_desc_t* app = esp_app_get_description();
  ESP_LOGI(TAG, "firmware %s (%s %s) running from %s", app->version, app->date, app->time,
           running ? running->label : "?");

  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    s_pendingVerify = true;
    esp_timer_create_args_t args = {};
    args.callback = verifyTimeout;
    args.name = "otaVerify";
    esp_timer_create(&args, &s_verifyTimer);
    esp_timer_start_once(s_verifyTimer, OTA_VERIFY_TIMEOUT_US);
    ESP_LOGW(TAG, "new firmware pending verification");
  }
}

void otaMarkHealthy() {
  if (!s_pendingVerify) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    s_pendingVerify = false;
    if (s_verifyTimer) esp_timer_stop(s_verifyTimer);
    ESP_LOGI(TAG, "new firmware marked valid");
  }
}

std::string otaRunningInfo() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  const esp_app_desc_t* app = esp_app_get_description();
  std::string info = std::string("Version ") + app->version + " (built " + app->date + " " + app->time + ")";
  info += std::string("<br>Running from ") + (running ? running->label : "?");
  if (next) {
    info += std::string(", update goes to ") + next->label + " (" +
            std::to_string(next->size / 1024) + " KB)";
  }
  return info;
}

bool otaBegin(size_t imageSize, std::string& err) {
  otaAbort();
  s_target = esp_ota_get_next_update_partition(nullptr);
  if (s_target == nullptr) {
    err = "No OTA partition available";
    return false;
  }
  if (imageSize > s_target->size) {
    err = "Image is larger than the OTA slot (" + std::to_string(s_target->size) + " bytes)";
    return false;
  }
  esp_err_t ret = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
  if (ret != ESP_OK) {
    err = std::string("esp_ota_begin failed: ") + esp_err_to_name(ret);
    s_handle = 0;
    return false;
  }
  s_written = 0;
  ESP_LOGI(TAG, "writing %u bytes to %s", (unsigned)imageSize, s_target->label);
  return true;
}

bool otaWrite(const void* data, size_t len, std::string& err) {
  esp_err_t ret = esp_ota_write(s_handle, data, len);
  if (ret != ESP_OK) {
    err = std::string("Write failed: ") + esp_err_to_name(ret) +
          (ret == ESP_ERR_OTA_VALIDATE_FAILED ? " (not an ESP32 firmware image?)" : "");
    otaAbort();
    return false;
  }
  s_written += len;
  return true;
}

bool otaFinish(std::string& err) {
  esp_err_t ret = esp_ota_end(s_handle);  // verifies the image checksum/hash
  s_handle = 0;
  if (ret != ESP_OK) {
    err = std::string("Image verification failed: ") + esp_err_to_name(ret);
    return false;
  }
  ret = esp_ota_set_boot_partition(s_target);
  if (ret != ESP_OK) {
    err = std::string("Set boot partition failed: ") + esp_err_to_name(ret);
    return false;
  }
  ESP_LOGI(TAG, "update written (%u bytes), rebooting into %s", (unsigned)s_written, s_target->label);

  // Restart from a timer so the HTTP response can still be sent.
  esp_timer_create_args_t args = {};
  args.callback = restartLater;
  args.name = "otaRestart";
  esp_timer_handle_t t = nullptr;
  esp_timer_create(&args, &t);
  esp_timer_start_once(t, 1500 * 1000);
  return true;
}

void otaAbort() {
  if (s_handle != 0) {
    esp_ota_abort(s_handle);
    s_handle = 0;
  }
}
