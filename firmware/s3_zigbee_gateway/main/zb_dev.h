#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ezbee/app_signals.h"
#include "ezbee/zcl.h"

#define ZB_DEV_MAX 16
#define ZB_EP_MAX 4

#define ZB_CAP_ONOFF (1u << 0)
#define ZB_CAP_LEVEL (1u << 1)
#define ZB_CAP_COLOR_XY (1u << 2)
#define ZB_CAP_COLOR_TEMP (1u << 3)

typedef struct {
    bool on;
    uint8_t level;      /* ZCL 0-254 */
    uint16_t x;         /* ZCL 0-65535 */
    uint16_t y;
    uint16_t mireds;
    uint16_t min_mireds; /* 设备 ColorTempPhysicalMinMireds */
    uint16_t max_mireds; /* 设备 ColorTempPhysicalMaxMireds */
    uint8_t color_mode;  /* 0=未知 1=XY 2=CT */
} zb_dev_state_t;

typedef struct {
    bool used;
    bool online;
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t ep;
    uint8_t caps;
    zb_dev_state_t state;
} zb_dev_t;

typedef void (*zb_dev_state_cb_t)(const zb_dev_t *dev);
typedef void (*zb_dev_discovered_cb_t)(const zb_dev_t *dev);

void zb_dev_restore(void);
void zb_dev_rescan(void);
void zb_dev_check_alive(void);
void zb_dev_set_state_cb(zb_dev_state_cb_t cb);
void zb_dev_set_discovered_cb(zb_dev_discovered_cb_t cb);
void zb_dev_note_onoff(uint16_t short_addr, bool on);
void zb_dev_note_level(uint16_t short_addr, uint8_t level);
void zb_dev_note_color_xy(uint16_t short_addr, uint16_t x, uint16_t y);
void zb_dev_note_color_temp(uint16_t short_addr, uint16_t mireds);
void zb_dev_handle_annce(const ezb_zdo_signal_device_annce_params_t *annce);
void zb_dev_handle_leave(const ezb_zdo_signal_leave_indication_params_t *leave);
void zb_dev_handle_read_rsp(const ezb_zcl_cmd_read_attr_rsp_message_t *rsp);
void zb_dev_handle_report(const ezb_zcl_cmd_report_attr_message_t *msg);
void zb_dev_dump(void);
uint8_t zb_dev_count(void);
const zb_dev_t *zb_dev_at(uint8_t index);
const zb_dev_t *zb_dev_find_ieee(uint64_t ieee);
const char *zb_dev_caps_str(uint8_t caps, char *buf, size_t len);
