#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "zb_dev.h"

esp_err_t zb_ctrl_set_onoff(const zb_dev_t *dev, bool on);
esp_err_t zb_ctrl_set_level(const zb_dev_t *dev, uint8_t level);
esp_err_t zb_ctrl_set_color_xy(const zb_dev_t *dev, uint16_t x, uint16_t y);
esp_err_t zb_ctrl_set_color_temp(const zb_dev_t *dev, uint16_t mireds);
esp_err_t zb_ctrl_configure_reporting(const zb_dev_t *dev);
esp_err_t zb_ctrl_bind_clusters(const zb_dev_t *dev);
