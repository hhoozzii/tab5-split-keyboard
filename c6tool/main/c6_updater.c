// One-time updater for the Tab5's ESP32-C6 (the Bluetooth/Wi-Fi co-processor).
// It sends the ESP-Hosted slave image that is embedded in this app to the C6 over the existing SDIO link,
// using ESP-Hosted's "host performs slave OTA". The C6 writes it to a spare slot and only switches over
// after the whole image was received and checked. Run it once, then flash the keyboard firmware again.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "bsp/m5stack_tab5.h"
#include "esp_hosted.h"
#if __has_include("esp_hosted_ota.h")
#include "esp_hosted_ota.h"
#else
esp_err_t esp_hosted_slave_ota_begin(void);
esp_err_t esp_hosted_slave_ota_write(uint8_t *ota_data, uint32_t ota_data_len);
esp_err_t esp_hosted_slave_ota_end(void);
esp_err_t esp_hosted_slave_ota_activate(void);
#endif

static const char *TAG = "c6upd";

extern const uint8_t img_start[] asm("_binary_network_adapter_esp32c6_bin_start");
extern const uint8_t img_end[] asm("_binary_network_adapter_esp32c6_bin_end");

#define CHUNK 1400

static void say(const char *s) { printf("\n=== %s ===\n\n", s); }

static void idle_forever(void) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void) {
  const uint32_t size = (uint32_t)(img_end - img_start);
  say("Tab5 C6 updater");

  // ---- check that the embedded file really is an ESP32-C6 app image, and read its version
  unsigned iv[3] = {0, 0, 0};
  bool haveVer = false;
  if (size < 4096 || img_start[0] != 0xE9) { say("STOP: the embedded file is not an ESP32 app image"); idle_forever(); }
  const uint16_t chip = (uint16_t)(img_start[12] | (img_start[13] << 8));
  if (chip != 13) { ESP_LOGE(TAG, "STOP: image is for chip id %u, an ESP32-C6 is 13", chip); idle_forever(); }
  // image header 24 bytes + first segment header 8 bytes, then esp_app_desc_t (magic 0xABCD5432, version at +16)
  const uint32_t magic = img_start[32] | (img_start[33] << 8) | (img_start[34] << 16) | ((uint32_t)img_start[35] << 24);
  char ver[33] = {0};
  if (magic == 0xABCD5432u) {
    memcpy(ver, img_start + 32 + 16, 32);
    haveVer = sscanf(ver, "%*[^0-9]%u.%u.%u", &iv[0], &iv[1], &iv[2]) == 3 || sscanf(ver, "%u.%u.%u", &iv[0], &iv[1], &iv[2]) == 3;
  }
  ESP_LOGI(TAG, "embedded C6 image: %lu bytes, chip id %u, version \"%s\"", (unsigned long)size, chip, ver);

  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }

  // ---- power up the C6 (same sequence as the keyboard firmware) and connect
  bsp_i2c_init();
  esp_err_t pe = bsp_feature_enable(BSP_FEATURE_WIFI, true);
  if (pe != ESP_OK) ESP_LOGW(TAG, "C6 power enable failed: %s", esp_err_to_name(pe));
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_hosted_connect_to_slave();

  esp_hosted_coprocessor_fwver_t cur;
  bool haveCur = esp_hosted_get_coprocessor_fwversion(&cur) == ESP_OK;
  if (haveCur) ESP_LOGI(TAG, "C6 currently runs firmware %lu.%lu.%lu", (unsigned long)cur.major1, (unsigned long)cur.minor1, (unsigned long)cur.patch1);
  else ESP_LOGW(TAG, "could not read the C6 firmware version");

  if (haveCur && haveVer && cur.major1 == iv[0] && cur.minor1 == iv[1] && cur.patch1 == iv[2]) {
    say("DONE: the C6 already runs this version. Flash the keyboard firmware again.");
    idle_forever();
  }

  // ---- last chance to abort
  for (int s = 10; s > 0; s--) {
    ESP_LOGW(TAG, "updating the C6 in %d s (unplug USB now to cancel)", s);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  say("sending the image to the C6. Do not unplug.");
  esp_err_t r = esp_hosted_slave_ota_begin();
  if (r != ESP_OK) {
    ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(r));
    say("STOP: this C6 firmware does not accept updates over the link (nothing was changed)");
    idle_forever();
  }
  uint32_t sent = 0, nextLog = 0;
  uint8_t buf[CHUNK];
  while (sent < size) {
    uint32_t n = size - sent < CHUNK ? size - sent : CHUNK;
    memcpy(buf, img_start + sent, n);   // the write call wants RAM, not flash-mapped data
    r = esp_hosted_slave_ota_write(buf, n);
    if (r != ESP_OK) {
      ESP_LOGE(TAG, "ota write failed at byte %lu: %s", (unsigned long)sent, esp_err_to_name(r));
      say("STOP: transfer failed. The C6 keeps its old firmware. Run this again, or send me the log.");
      idle_forever();
    }
    sent += n;
    if (sent >= nextLog) { ESP_LOGI(TAG, "sent %lu%%", (unsigned long)(sent * 100ULL / size)); nextLog = sent + size / 10; }
  }
  r = esp_hosted_slave_ota_end();
  if (r != ESP_OK) {
    ESP_LOGE(TAG, "ota end (verify) failed: %s", esp_err_to_name(r));
    say("STOP: the C6 rejected the image. It keeps its old firmware.");
    idle_forever();
  }
  ESP_LOGI(TAG, "image accepted, switching the C6 over");
  r = esp_hosted_slave_ota_activate();
  if (r != ESP_OK) {
    ESP_LOGW(TAG, "activate returned %s", esp_err_to_name(r));
    say("The C6 may not support switching remotely. Power-cycle the Tab5 and run this again to see the version.");
    idle_forever();
  }
  say("DONE. The C6 is restarting on the new firmware.");
  ESP_LOGI(TAG, "Now flash the keyboard firmware again (idf.py -p COMx build flash monitor in the firmware folder).");
  ESP_LOGI(TAG, "Its boot log shows the C6 version; it should read 2.12.13.");
  idle_forever();   // no automatic restart: if the version could not be compared, a restart would update again
}
