#pragma once

#include <stddef.h>

#include "esp_err.h"

/* 已进入 SoftAP 配网状态，调用方应停在此处等用户提交凭据 */
#define WIFI_PROV_AP_MODE ESP_ERR_INVALID_STATE

esp_err_t wifi_prov_connect(void);

/* 读取配网时填写的 MQTT 地址，未配置返回 ESP_ERR_NOT_FOUND */
esp_err_t wifi_prov_get_mqtt_uri(char *out, size_t len);
