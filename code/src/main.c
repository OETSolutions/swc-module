#include <stdio.h>
#include <inttypes.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "swc-boot";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    ESP_LOGI(TAG, "chip=%s cores=%d rev=v%d.%d",
             CONFIG_IDF_TARGET, chip.cores, chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "flash=%" PRIu32 " bytes", flash_size);
    ESP_LOGI(TAG, "idf=%s", esp_get_idf_version());

    if (flash_size != 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "flash is %" PRIu32 " bytes, build assumes %d", flash_size, 4 * 1024 * 1024);
        abort();
    }
    if (chip.cores != 2) {
        ESP_LOGE(TAG, "expected 2 cores for ESP32-S3, got %d", chip.cores);
        abort();
    }
    ESP_LOGI(TAG, "board definition verified: 4MB flash, %d cores, IDF %s",
             chip.cores, esp_get_idf_version());
}
