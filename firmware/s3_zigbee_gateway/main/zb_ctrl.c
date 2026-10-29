#include "zb_ctrl.h"

#include <string.h>

#include "esp_log.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl.h"
#include "ezbee/zdo.h"

#include "zigbee_gateway.h"

#define TAG "ZB_CTRL"

#define GW_SRC_EP ESP_ZIGBEE_CUSTOM_GATEWAY_EP_ID

static void fill_ctrl(ezb_zcl_cluster_cmd_ctrl_t *ctrl, const zb_dev_t *dev)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    ctrl->dst_addr.u.short_addr = dev->short_addr;
    ctrl->dst_ep = dev->ep;
    ctrl->src_ep = GW_SRC_EP;
    ctrl->dis_default_rsp = false;
}

static void fill_gen_ctrl(ezb_zcl_cmd_ctrl_t *ctrl, const zb_dev_t *dev, uint16_t cluster_id)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    ctrl->dst_addr.u.short_addr = dev->short_addr;
    ctrl->dst_ep = dev->ep;
    ctrl->src_ep = GW_SRC_EP;
    ctrl->cluster_id = cluster_id;
    ctrl->fc.direction = 0;
    ctrl->fc.dis_default_rsp = 0;
}

static esp_err_t result(ezb_err_t err, const char *what, const zb_dev_t *dev)
{
    if (err == EZB_ERR_NONE) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "%s 到 0x%04x ep%d 失败: %d", what, dev->short_addr, dev->ep, err);
    return ESP_FAIL;
}

esp_err_t zb_ctrl_set_onoff(const zb_dev_t *dev, bool on)
{
    ezb_zcl_on_off_cmd_t cmd = {0};

    fill_ctrl(&cmd.cmd_ctrl, dev);
    const ezb_err_t err = on ? ezb_zcl_on_off_on_cmd_req(&cmd) : ezb_zcl_on_off_off_cmd_req(&cmd);
    ESP_LOGI(TAG, "下发 OnOff=%s 到 0x%04x ep%d", on ? "ON" : "OFF", dev->short_addr, dev->ep);
    if (err == EZB_ERR_NONE) {
        zb_dev_note_onoff(dev->short_addr, on);
    }
    return result(err, on ? "On" : "Off", dev);
}

esp_err_t zb_ctrl_set_level(const zb_dev_t *dev, uint8_t level)
{
    ezb_zcl_level_move_to_level_with_on_off_cmd_t cmd = {0};

    fill_ctrl(&cmd.cmd_ctrl, dev);
    cmd.payload.level = level;
    cmd.payload.transition_time = 0;
    const ezb_err_t err = ezb_zcl_level_move_to_level_with_on_off_cmd_req(&cmd);
    ESP_LOGI(TAG, "下发 Level=%d 到 0x%04x ep%d", level, dev->short_addr, dev->ep);
    if (err == EZB_ERR_NONE) {
        zb_dev_note_level(dev->short_addr, level);
        zb_dev_note_onoff(dev->short_addr, true);
    }
    return result(err, "MoveToLevelWithOnOff", dev);
}

esp_err_t zb_ctrl_set_color_xy(const zb_dev_t *dev, uint16_t x, uint16_t y)
{
    ezb_zcl_color_control_move_to_color_cmd_t cmd = {0};

    fill_ctrl(&cmd.cmd_ctrl, dev);
    cmd.payload.color_x = x;
    cmd.payload.color_y = y;
    cmd.payload.transition_time = 0;
    const ezb_err_t err = ezb_zcl_color_control_move_to_color_cmd_req(&cmd);
    ESP_LOGI(TAG, "下发 ColorXY=(%u,%u) 到 0x%04x ep%d", x, y, dev->short_addr, dev->ep);
    if (err == EZB_ERR_NONE) {
        zb_dev_note_color_xy(dev->short_addr, x, y);
    }
    return result(err, "MoveToColor", dev);
}

esp_err_t zb_ctrl_set_color_temp(const zb_dev_t *dev, uint16_t mireds)
{
    ezb_zcl_color_control_move_to_color_temperature_cmd_t cmd = {0};

    fill_ctrl(&cmd.cmd_ctrl, dev);
    cmd.payload.color_temperature_mireds = mireds;
    cmd.payload.transition_time = 0;
    const ezb_err_t err = ezb_zcl_color_control_move_to_color_temperature_cmd_req(&cmd);
    ESP_LOGI(TAG, "下发 ColorTemp=%u mireds 到 0x%04x ep%d", mireds, dev->short_addr, dev->ep);
    if (err == EZB_ERR_NONE) {
        zb_dev_note_color_temp(dev->short_addr, mireds);
    }
    return result(err, "MoveToColorTemperature", dev);
}

static void bind_cb(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    const uint16_t cluster_id = (uint16_t)(uintptr_t)user_ctx;

    if (result == NULL || result->error != EZB_ERR_NONE || result->rsp == NULL ||
        result->rsp->status != 0) {
        ESP_LOGW(TAG, "绑定 cluster 0x%04x 失败", cluster_id);
        return;
    }
    ESP_LOGI(TAG, "绑定 cluster 0x%04x 成功", cluster_id);
}

esp_err_t zb_ctrl_bind_clusters(const zb_dev_t *dev)
{
    static ezb_zdo_bind_req_t s_bind_req[3];
    static const uint16_t caps[3] = {ZB_CAP_ONOFF, ZB_CAP_LEVEL, ZB_CAP_COLOR_XY | ZB_CAP_COLOR_TEMP};
    static const uint16_t clusters[3] = {EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_ID_LEVEL,
                                         EZB_ZCL_CLUSTER_ID_COLOR_CONTROL};
    ezb_extaddr_t self;
    int done = 0;

    ezb_nwk_get_extended_address(&self);
    for (int i = 0; i < 3; i++) {
        if ((dev->caps & caps[i]) == 0) {
            continue;
        }
        ezb_zdo_bind_req_t *req = &s_bind_req[i];
        memset(req, 0, sizeof(*req));
        req->dst_nwk_addr = dev->short_addr;
        req->field.src_addr.u64 = dev->ieee;
        req->field.src_ep = dev->ep;
        req->field.cluster_id = clusters[i];
        req->field.dst_addr_mode = 0x03;
        req->field.dst_addr.extended_addr = self;
        req->field.dst_ep = GW_SRC_EP;
        req->cb = bind_cb;
        req->user_ctx = (void *)(uintptr_t)clusters[i];
        if (ezb_zdo_bind_req(req) != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "绑定请求 cluster 0x%04x 发送失败", clusters[i]);
            continue;
        }
        done++;
    }
    return done > 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t config_report(const zb_dev_t *dev, uint16_t cluster_id, uint16_t attr_id,
                               uint8_t attr_type, ezb_zcl_attr_variable_t change)
{
    ezb_zcl_config_report_cmd_t cmd = {0};
    ezb_zcl_config_report_record_t rec = {
        .direction = EZB_ZCL_REPORTING_SEND,
        .attr_id = attr_id,
        .client =
            {
                .attr_type = attr_type,
                .min_interval = 1,
                .max_interval = 3600,
                .reportable_change = change,
            },
    };

    fill_gen_ctrl(&cmd.cmd_ctrl, dev, cluster_id);
    cmd.payload.record_number = 1;
    cmd.payload.record_field = &rec;
    const ezb_err_t err = ezb_zcl_config_report_cmd_req(&cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "配置上报 cluster 0x%04x attr 0x%04x 失败: %d", cluster_id, attr_id, err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t zb_ctrl_configure_reporting(const zb_dev_t *dev)
{
    int ok = 0;

    if (dev->caps & ZB_CAP_ONOFF) {
        ok += (config_report(dev, EZB_ZCL_CLUSTER_ID_ON_OFF, 0x0000, EZB_ZCL_ATTR_TYPE_BOOL,
                             (ezb_zcl_attr_variable_t){.u8 = 1}) == ESP_OK);
    }
    if (dev->caps & ZB_CAP_LEVEL) {
        ok += (config_report(dev, EZB_ZCL_CLUSTER_ID_LEVEL, 0x0000, EZB_ZCL_ATTR_TYPE_UINT8,
                             (ezb_zcl_attr_variable_t){.u8 = 1}) == ESP_OK);
    }
    if (dev->caps & ZB_CAP_COLOR_XY) {
        ok += (config_report(dev, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0003,
                             EZB_ZCL_ATTR_TYPE_UINT16, (ezb_zcl_attr_variable_t){.u16 = 1}) == ESP_OK);
        ok += (config_report(dev, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0004,
                             EZB_ZCL_ATTR_TYPE_UINT16, (ezb_zcl_attr_variable_t){.u16 = 1}) == ESP_OK);
    }
    if (dev->caps & ZB_CAP_COLOR_TEMP) {
        ok += (config_report(dev, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0007,
                             EZB_ZCL_ATTR_TYPE_UINT16, (ezb_zcl_attr_variable_t){.u16 = 1}) == ESP_OK);
    }
    if (dev->caps & (ZB_CAP_COLOR_XY | ZB_CAP_COLOR_TEMP)) {
        ok += (config_report(dev, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, 0x0008,
                             EZB_ZCL_ATTR_TYPE_ENUM8, (ezb_zcl_attr_variable_t){.u8 = 1}) == ESP_OK);
    }
    ESP_LOGI(TAG, "0x%04x 上报配置完成 %d 项", dev->short_addr, ok);
    return ok > 0 ? ESP_OK : ESP_FAIL;
}
