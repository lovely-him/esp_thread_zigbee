#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rcp_update.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mode_select.h"

#define TAG "MODE_SEL"

#define PIN_BOOT         0
#define PIN_RCP_RESET    7
#define PIN_RCP_BOOT     8
#define PIN_RCP_RX       17
#define PIN_RCP_TX       18
#define RCP_UART_PORT    1

#define DELAY_BEFORE_MS  1000
#define WINDOW_MS        5000
#define LONG_PRESS_MS    2000
#define POLL_MS          20
#define RCP_RETRY_MAX    3

#define RCP_PARTITION    "rcp_fw"
#define RCP_MOUNT_POINT  "/" RCP_PARTITION
#define RCP_FIRMWARE_DIR RCP_MOUNT_POINT "/rcp"
#define RCP_SEQ_NS       "storage"
#define RCP_SEQ_KEY      "rcp_seq"
#define RCP_SEQ_VERIFIED 0x20

#define NVS_NS           "bootcfg"
#define KEY_MODE         "mode"
#define KEY_H2_MODE      "h2_mode"
#define KEY_H2_VER       "h2_ver"
#define KEY_RETRY        "retry"

#define VERSION_MAX      96
#define H2_MODE_UNKNOWN  (-1)

// ── NVS ──

static int8_t load_i8(const char *key, int8_t fallback)
{
    nvs_handle_t h;
    int8_t v = fallback;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return fallback;
    }
    if (nvs_get_i8(h, key, &v) != ESP_OK) {
        v = fallback;
    }
    nvs_close(h);
    return v;
}

static void store_i8(const char *key, int8_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void load_h2_version(char *out, size_t len)
{
    nvs_handle_t h;
    size_t size = len;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_str(h, KEY_H2_VER, out, &size) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(h);
}

static void store_h2_state(int idx, const char *version)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i8(h, KEY_H2_MODE, (int8_t)idx);
    nvs_set_str(h, KEY_H2_VER, version);
    nvs_commit(h);
    nvs_close(h);
}

// ── 按钮窗口 ──

static bool button_window(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    vTaskDelay(pdMS_TO_TICKS(DELAY_BEFORE_MS));
    ESP_LOGI(TAG, "S3 开始等待按钮");

    const int64_t window_end = esp_timer_get_time() + (int64_t)WINDOW_MS * 1000;
    int64_t low_since = 0;

    while (esp_timer_get_time() < window_end) {
        if (gpio_get_level(PIN_BOOT) == 0) {
            const int64_t now = esp_timer_get_time();
            if (low_since == 0) {
                low_since = now;
            } else if (now - low_since >= (int64_t)LONG_PRESS_MS * 1000) {
                return true;
            }
        } else {
            low_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    ESP_LOGI(TAG, "S3 结束等待按钮");
    return false;
}

// ── H2 固件 ──

static esp_err_t mount_spiffs(void)
{
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = RCP_MOUNT_POINT,
        .partition_label = RCP_PARTITION,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    return (err == ESP_ERR_INVALID_STATE) ? ESP_OK : err;
}

static esp_err_t set_rcp_seq(int idx)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RCP_SEQ_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i8(h, RCP_SEQ_KEY, (int8_t)(idx | RCP_SEQ_VERIFIED));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t ensure_rcp(app_mode_t mode)
{
    const int idx = (mode == APP_MODE_ZIGBEE) ? 1 : 0;
    const esp_rcp_update_config_t cfg = {
        .rcp_type = RCP_TYPE_UART,
        .uart_rx_pin = PIN_RCP_RX,
        .uart_tx_pin = PIN_RCP_TX,
        .uart_port = RCP_UART_PORT,
        .uart_baudrate = 115200,
        .reset_pin = PIN_RCP_RESET,
        .boot_pin = PIN_RCP_BOOT,
        .update_baudrate = 460800,
        .firmware_dir = RCP_FIRMWARE_DIR,
        .target_chip = ESP32H2_CHIP,
    };
    char img_ver[VERSION_MAX] = {0};
    char cur_ver[VERSION_MAX] = {0};

    esp_err_t err = mount_spiffs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 %s 失败：%s", RCP_PARTITION, esp_err_to_name(err));
        return err;
    }
    err = set_rcp_seq(idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写 %s 失败：%s", RCP_SEQ_KEY, esp_err_to_name(err));
        return err;
    }
    err = esp_rcp_update_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_rcp_update_init 失败：%s", esp_err_to_name(err));
        return err;
    }

    if (esp_rcp_load_version_in_storage(img_ver, sizeof(img_ver)) != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 中缺少 rcp_%d 镜像", idx);
        esp_rcp_update_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    load_h2_version(cur_ver, sizeof(cur_ver));
    if (load_i8(KEY_H2_MODE, H2_MODE_UNKNOWN) == idx && strcmp(cur_ver, img_ver) == 0) {
        ESP_LOGI(TAG, "H2 已匹配 rcp_%d：%s", idx, img_ver);
        esp_rcp_update_deinit();
        return ESP_OK;
    }

    ESP_LOGW(TAG, "刷写 H2 → rcp_%d（%s）", idx, img_ver);
    err = esp_rcp_update();
    esp_rcp_update_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "刷写 H2 失败：%s", esp_err_to_name(err));
        return err;
    }
    store_h2_state(idx, img_ver);
    ESP_LOGI(TAG, "H2 刷写完成");
    return ESP_OK;
}

// ── app 分区 ──

static void switch_app_if_needed(app_mode_t mode)
{
    const esp_partition_subtype_t need = (mode == APP_MODE_ZIGBEE)
                                             ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                             : ESP_PARTITION_SUBTYPE_APP_FACTORY;
    const esp_partition_t *target =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, need, NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (target == NULL) {
        ESP_LOGE(TAG, "找不到目标 app 分区（subtype=%d）", need);
        return;
    }
    if (running != NULL && running->address == target->address) {
        return;
    }
    ESP_LOGW(TAG, "切换 app → %s", target->label);
    ESP_ERROR_CHECK(esp_ota_set_boot_partition(target));
    esp_restart();
}

// ── 主流程 ──

esp_err_t mode_select_run(void)
{
    app_mode_t mode = (load_i8(KEY_MODE, APP_MODE_THREAD) == APP_MODE_ZIGBEE)
                          ? APP_MODE_ZIGBEE
                          : APP_MODE_THREAD;

    if (button_window()) {
        mode = (mode == APP_MODE_THREAD) ? APP_MODE_ZIGBEE : APP_MODE_THREAD;
        store_i8(KEY_MODE, (int8_t)mode);
        ESP_LOGW(TAG, "按钮触发切换 → %s", mode == APP_MODE_ZIGBEE ? "Zigbee" : "Thread");
    }

    esp_err_t err = ensure_rcp(mode);
    if (err != ESP_OK) {
        const int8_t retry = load_i8(KEY_RETRY, 0) + 1;
        store_i8(KEY_RETRY, retry);
        ESP_LOGE(TAG, "H2 固件准备失败（第 %d 次）", retry);
        if (retry >= RCP_RETRY_MAX) {
            store_i8(KEY_RETRY, 0);
            ESP_LOGE(TAG, "已达重试上限，停在当前 app 与当前模式");
        }
        return err;
    }
    store_i8(KEY_RETRY, 0);

    switch_app_if_needed(mode);
    return ESP_OK;
}
