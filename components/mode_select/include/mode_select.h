#pragma once

#include "esp_err.h"

typedef enum {
    APP_MODE_THREAD = 0,
    APP_MODE_ZIGBEE = 1,
} app_mode_t;

/**
 * 执行启动流程：按钮窗口 → 确定目标模式 → 保证 H2 固件匹配 → 切到目标 app。
 * 需要切换 app 或刷写失败时不会返回。
 *
 * @return ESP_OK  当前 app 就是目标模式的 app（Thread 模式下 factory 即终态）
 */
esp_err_t mode_select_run(void);
