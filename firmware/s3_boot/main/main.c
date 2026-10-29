#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "mode_select.h"

#define TAG "S3_BOOT"

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    nvs_init();

    const esp_err_t err = mode_select_run();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动流程未完成：%s", esp_err_to_name(err));
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
