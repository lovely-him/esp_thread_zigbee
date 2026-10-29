#include "zb_dev.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ezbee/zdo.h"
#include "nvs.h"

#include "zigbee_gateway.h"

#define TAG "ZB_DEV"

#define NVS_NAMESPACE "zigbee"
#define NVS_KEY_DEVS "devs"
#define STORE_VERSION 1

#define GW_SRC_EP ESP_ZIGBEE_CUSTOM_GATEWAY_EP_ID

#define PROBE_TIMEOUT_MS 8000

#define SLOT_ONOFF 0
#define SLOT_LEVEL 1
#define SLOT_COLOR 2
#define SLOT_COUNT 3

typedef struct {
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t ep;
    uint8_t caps;
} zb_dev_rec_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    zb_dev_rec_t rec[ZB_DEV_MAX];
} zb_store_t;

static zb_dev_t s_devs[ZB_DEV_MAX];
static int64_t s_probe_ms[ZB_DEV_MAX];
static uint8_t s_last_cmd_mode[ZB_DEV_MAX];
static ezb_zdo_active_ep_req_t s_ep_req[ZB_DEV_MAX];
static ezb_zdo_simple_desc_req_t s_desc_req[ZB_DEV_MAX][ZB_EP_MAX];
static ezb_zcl_read_attr_cmd_t s_read_req[ZB_DEV_MAX][SLOT_COUNT];
static zb_dev_state_cb_t s_state_cb;
static zb_dev_discovered_cb_t s_discovered_cb;
static bool s_reporting_cfg[ZB_DEV_MAX];

static uint16_t s_attr_onoff[] = {0x0000};
static uint16_t s_attr_level[] = {0x0000};
static uint16_t s_attr_color[] = {0x0003, 0x0004, 0x0007, 0x0008, 0x400b, 0x400c};

#define DEFAULT_MIN_MIREDS 153
#define DEFAULT_MAX_MIREDS 500

static void notify_state(const zb_dev_t *dev)
{
    if (s_state_cb != NULL) {
        s_state_cb(dev);
    }
}

static int dev_find_idx_ieee(uint64_t ieee)
{
    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (s_devs[i].used && s_devs[i].ieee == ieee) {
            return i;
        }
    }
    return -1;
}

static int dev_find_idx_short(uint16_t short_addr)
{
    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (s_devs[i].used && s_devs[i].short_addr == short_addr) {
            return i;
        }
    }
    return -1;
}

static int dev_alloc(void)
{
    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (!s_devs[i].used) {
            memset(&s_devs[i], 0, sizeof(s_devs[i]));
            s_devs[i].used = true;
            s_devs[i].state.min_mireds = DEFAULT_MIN_MIREDS;
            s_devs[i].state.max_mireds = DEFAULT_MAX_MIREDS;
            return i;
        }
    }
    return -1;
}

static void dev_save(void)
{
    zb_store_t store = {.version = STORE_VERSION, .count = 0};
    nvs_handle_t handle;

    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (!s_devs[i].used) {
            continue;
        }
        zb_dev_rec_t *rec = &store.rec[store.count++];
        rec->ieee = s_devs[i].ieee;
        rec->short_addr = s_devs[i].short_addr;
        rec->ep = s_devs[i].ep;
        rec->caps = s_devs[i].caps;
    }

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(handle, NVS_KEY_DEVS, &store, sizeof(store)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

void zb_dev_restore(void)
{
    zb_store_t store;
    size_t len = sizeof(store);
    nvs_handle_t handle;

    memset(s_devs, 0, sizeof(s_devs));
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    const esp_err_t err = nvs_get_blob(handle, NVS_KEY_DEVS, &store, &len);
    nvs_close(handle);

    if (err != ESP_OK || store.version != STORE_VERSION || store.count > ZB_DEV_MAX) {
        return;
    }
    for (int i = 0; i < store.count; i++) {
        s_devs[i].used = true;
        s_devs[i].online = false;
        s_devs[i].ieee = store.rec[i].ieee;
        s_devs[i].short_addr = store.rec[i].short_addr;
        s_devs[i].ep = store.rec[i].ep;
        s_devs[i].caps = store.rec[i].caps;
    }
    ESP_LOGI(TAG, "从 NVS 恢复 %d 个设备", store.count);
}

void zb_dev_set_state_cb(zb_dev_state_cb_t cb)
{
    s_state_cb = cb;
}

void zb_dev_set_discovered_cb(zb_dev_discovered_cb_t cb)
{
    s_discovered_cb = cb;
}

void zb_dev_note_onoff(uint16_t short_addr, bool on)
{
    const int idx = dev_find_idx_short(short_addr);

    if (idx < 0 || s_devs[idx].state.on == on) {
        return;
    }
    s_devs[idx].state.on = on;
    notify_state(&s_devs[idx]);
}

void zb_dev_note_level(uint16_t short_addr, uint8_t level)
{
    const int idx = dev_find_idx_short(short_addr);

    if (idx < 0) {
        return;
    }
    s_devs[idx].state.level = level;
    notify_state(&s_devs[idx]);
}

void zb_dev_note_color_xy(uint16_t short_addr, uint16_t x, uint16_t y)
{
    const int idx = dev_find_idx_short(short_addr);

    if (idx < 0) {
        return;
    }
    s_last_cmd_mode[idx] = 1;
    s_devs[idx].state.x = x;
    s_devs[idx].state.y = y;
    s_devs[idx].state.color_mode = 1;
    notify_state(&s_devs[idx]);
}

void zb_dev_note_color_temp(uint16_t short_addr, uint16_t mireds)
{
    const int idx = dev_find_idx_short(short_addr);

    if (idx < 0) {
        return;
    }
    s_last_cmd_mode[idx] = 2;
    s_devs[idx].state.mireds = mireds;
    s_devs[idx].state.color_mode = 2;
    notify_state(&s_devs[idx]);
}

const zb_dev_t *zb_dev_find_ieee(uint64_t ieee)
{
    const int idx = dev_find_idx_ieee(ieee);
    return (idx >= 0) ? &s_devs[idx] : NULL;
}

const char *zb_dev_caps_str(uint8_t caps, char *buf, size_t len)
{
    snprintf(buf, len, "%s%s%s%s", (caps & ZB_CAP_ONOFF) ? "onoff " : "",
             (caps & ZB_CAP_LEVEL) ? "level " : "", (caps & ZB_CAP_COLOR_XY) ? "xy " : "",
             (caps & ZB_CAP_COLOR_TEMP) ? "ct " : "");
    return buf;
}

static uint8_t caps_from_clusters(const uint16_t *clusters, uint8_t count)
{
    uint8_t caps = 0;

    for (uint8_t i = 0; i < count; i++) {
        if (clusters[i] == EZB_ZCL_CLUSTER_ID_ON_OFF) {
            caps |= ZB_CAP_ONOFF;
        } else if (clusters[i] == EZB_ZCL_CLUSTER_ID_LEVEL) {
            caps |= ZB_CAP_LEVEL;
        }
    }
    return caps;
}

static void request_attr(int idx, uint8_t slot, uint8_t ep, uint16_t cluster, uint16_t *attr_ids,
                         uint8_t attr_number)
{
    zb_dev_t *dev = &s_devs[idx];
    ezb_zcl_read_attr_cmd_t *req = &s_read_req[idx][slot];

    memset(req, 0, sizeof(*req));
    req->cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    req->cmd_ctrl.dst_addr.u.short_addr = dev->short_addr;
    req->cmd_ctrl.dst_ep = ep;
    req->cmd_ctrl.src_ep = GW_SRC_EP;
    req->cmd_ctrl.cluster_id = cluster;
    req->cmd_ctrl.fc.direction = 0;
    req->cmd_ctrl.fc.dis_default_rsp = 1;
    req->payload.attr_number = attr_number;
    req->payload.attr_field = attr_ids;

    if (ezb_zcl_read_attr_cmd_req(req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "0x%04x 读取 cluster 0x%04x 失败", dev->short_addr, cluster);
    }
}

static void simple_desc_cb(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx)
{
    const int idx = (int)(uintptr_t)user_ctx - 1;
    zb_dev_t *dev;
    char caps_buf[32];

    if (idx < 0 || idx >= ZB_DEV_MAX || !s_devs[idx].used) {
        return;
    }
    dev = &s_devs[idx];
    if (result == NULL || result->error != EZB_ERR_NONE || result->rsp == NULL) {
        ESP_LOGW(TAG, "0x%04x 简单描述符查询失败", dev->short_addr);
        return;
    }

    const ezb_af_simple_desc_t *desc = &result->rsp->desc;
    ESP_LOGI(TAG, "  端点 %d profile=0x%04x device=0x%04x", desc->ep_id, desc->app_profile_id,
             desc->app_device_id);
    const uint8_t old_ep = dev->ep;

    const uint8_t caps = caps_from_clusters(desc->app_cluster_list, desc->app_input_cluster_count);
    const uint8_t old_caps = dev->caps;
    dev->caps |= caps;
    if ((caps & ZB_CAP_ONOFF) && dev->ep == 0) {
        dev->ep = desc->ep_id;
    }
    if (dev->caps != old_caps || dev->ep != old_ep) {
        dev_save();
    }

    ESP_LOGI(TAG, "0x%04x ieee=0x%016llx ep=%d caps=%s", dev->short_addr,
             (unsigned long long)dev->ieee, dev->ep,
             zb_dev_caps_str(dev->caps, caps_buf, sizeof(caps_buf)));

    if (desc->app_profile_id != EZB_AF_HA_PROFILE_ID) {
        return;
    }
    if (!s_reporting_cfg[idx] && s_discovered_cb != NULL) {
        s_reporting_cfg[idx] = true;
        s_discovered_cb(dev);
    }
    if (caps & ZB_CAP_ONOFF) {
        request_attr(idx, SLOT_ONOFF, desc->ep_id, EZB_ZCL_CLUSTER_ID_ON_OFF, s_attr_onoff, 1);
    }
    if (caps & ZB_CAP_LEVEL) {
        request_attr(idx, SLOT_LEVEL, desc->ep_id, EZB_ZCL_CLUSTER_ID_LEVEL, s_attr_level, 1);
    }
    for (uint8_t i = 0; i < desc->app_input_cluster_count; i++) {
        if (desc->app_cluster_list[i] == EZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
            request_attr(idx, SLOT_COLOR, desc->ep_id, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                         s_attr_color, sizeof(s_attr_color) / sizeof(s_attr_color[0]));
            break;
        }
    }
}

static void active_ep_cb(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    const int idx = (int)(uintptr_t)user_ctx - 1;
    zb_dev_t *dev;

    if (idx < 0 || idx >= ZB_DEV_MAX || !s_devs[idx].used) {
        return;
    }
    dev = &s_devs[idx];
    if (result == NULL || result->error != EZB_ERR_NONE || result->rsp == NULL) {
        ESP_LOGW(TAG, "0x%04x 活动端点查询失败", dev->short_addr);
        return;
    }

    const bool was_online = dev->online;
    dev->online = true;
    s_probe_ms[idx] = 0;
    if (!was_online) {
        ESP_LOGI(TAG, "0x%04x 在线（探活成功）", dev->short_addr);
        notify_state(dev);
    }

    ESP_LOGI(TAG, "0x%04x 活动端点数=%d", dev->short_addr, result->rsp->active_ep_count);
    for (uint8_t i = 0; i < result->rsp->active_ep_count && i < ZB_EP_MAX; i++) {
        ezb_zdo_simple_desc_req_t *req = &s_desc_req[idx][i];

        memset(req, 0, sizeof(*req));
        req->dst_nwk_addr = dev->short_addr;
        req->field.nwk_addr_of_interest = dev->short_addr;
        req->field.endpoint = result->rsp->active_ep_list[i];
        req->cb = simple_desc_cb;
        req->user_ctx = (void *)(uintptr_t)(idx + 1);
        if (ezb_zdo_simple_desc_req(req) != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "  端点 %d 描述符请求发送失败", result->rsp->active_ep_list[i]);
        }
    }
}

static void start_discovery(int idx)
{
    zb_dev_t *dev = &s_devs[idx];

    memset(&s_ep_req[idx], 0, sizeof(s_ep_req[idx]));
    s_ep_req[idx].dst_nwk_addr = dev->short_addr;
    s_ep_req[idx].field.nwk_addr_of_interest = dev->short_addr;
    s_ep_req[idx].cb = active_ep_cb;
    s_ep_req[idx].user_ctx = (void *)(uintptr_t)(idx + 1);
    if (ezb_zdo_active_ep_req(&s_ep_req[idx]) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "0x%04x 活动端点请求发送失败", dev->short_addr);
    }
}

void zb_dev_handle_annce(const ezb_zdo_signal_device_annce_params_t *annce)
{
    int idx;
    zb_dev_t *dev;

    if (annce == NULL) {
        return;
    }
    ESP_LOGI(TAG, "入网 short=0x%04x ieee=0x%016llx", annce->short_addr,
             (unsigned long long)annce->device_addr.u64);

    idx = dev_find_idx_ieee(annce->device_addr.u64);
    if (idx < 0) {
        idx = dev_alloc();
        if (idx < 0) {
            ESP_LOGE(TAG, "设备表已满（%d）", ZB_DEV_MAX);
            return;
        }
        s_devs[idx].ieee = annce->device_addr.u64;
    }
    dev = &s_devs[idx];
    if (dev->short_addr != annce->short_addr) {
        if (dev->short_addr != 0) {
            ESP_LOGW(TAG, "ieee=0x%016llx 的 short 由 0x%04x 变为 0x%04x",
                     (unsigned long long)dev->ieee, dev->short_addr, annce->short_addr);
        }
        dev->short_addr = annce->short_addr;
    }
    dev->online = true;
    s_probe_ms[idx] = 0;
    dev_save();

    start_discovery(idx);
}

void zb_dev_rescan(void)
{
    int n = 0;
    const int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (!s_devs[i].used || s_devs[i].short_addr == 0) {
            continue;
        }
        s_probe_ms[i] = now_ms;
        start_discovery(i);
        n++;
    }
    if (n > 0) {
        ESP_LOGI(TAG, "探活 %d 个已记录设备", n);
    }
}

void zb_dev_check_alive(void)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (!s_devs[i].used || s_probe_ms[i] == 0) {
            continue;
        }
        if (now_ms - s_probe_ms[i] < PROBE_TIMEOUT_MS) {
            continue;
        }
        s_probe_ms[i] = 0;
        if (s_devs[i].online) {
            s_devs[i].online = false;
            ESP_LOGW(TAG, "0x%04x 探活超时，标记离线", s_devs[i].short_addr);
            notify_state(&s_devs[i]);
        }
    }
}

void zb_dev_handle_leave(const ezb_zdo_signal_leave_indication_params_t *leave)
{
    const int idx = (leave == NULL) ? -1 : dev_find_idx_short(leave->short_addr);

    if (idx < 0) {
        return;
    }
    s_devs[idx].online = false;
    ESP_LOGW(TAG, "设备离线 ieee=0x%016llx short=0x%04x", (unsigned long long)s_devs[idx].ieee,
             leave->short_addr);
    notify_state(&s_devs[idx]);
}

static void update_attr(zb_dev_t *dev, uint16_t cluster, uint16_t attr_id, const void *value,
                        bool *changed, bool *xy_changed, bool *ct_changed)
{
    zb_dev_state_t *st = &dev->state;
    const int idx = (int)(dev - s_devs);

    if (cluster == EZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == 0x0000) {
        const bool on = *(const uint8_t *)value != 0;
        *changed |= (st->on != on);
        st->on = on;
    } else if (cluster == EZB_ZCL_CLUSTER_ID_LEVEL && attr_id == 0x0000) {
        const uint8_t level = *(const uint8_t *)value;
        *changed |= (st->level != level);
        st->level = level;
    } else if (cluster == EZB_ZCL_CLUSTER_ID_COLOR_CONTROL) {
        const uint16_t v = *(const uint16_t *)value;
        switch (attr_id) {
        case 0x0003:
            *xy_changed |= (st->x != v);
            st->x = v;
            dev->caps |= ZB_CAP_COLOR_XY;
            break;
        case 0x0004:
            *xy_changed |= (st->y != v);
            st->y = v;
            dev->caps |= ZB_CAP_COLOR_XY;
            break;
        case 0x0007:
            *ct_changed |= (st->mireds != v);
            st->mireds = v;
            dev->caps |= ZB_CAP_COLOR_TEMP;
            break;
        case 0x400b:
            st->min_mireds = v;
            break;
        case 0x400c:
            st->max_mireds = v;
            break;
        case 0x0008: {
            const uint8_t reported = *(const uint8_t *)value;
            if (st->color_mode != 0 && reported != st->color_mode) {
                ESP_LOGW(TAG, "0x%04x ColorMode 回报 %u 与推导值 %u 不一致（最近命令：%s）",
                         dev->short_addr, reported, st->color_mode,
                         s_last_cmd_mode[idx] == 1   ? "MoveToColor"
                         : s_last_cmd_mode[idx] == 2 ? "MoveToColorTemperature"
                                                     : "无");
            }
            break;
        }
        default:
            break;
        }
    }
}

static bool apply_color_mode(zb_dev_t *dev, bool xy_changed, bool ct_changed)
{
    uint8_t mode = dev->state.color_mode;

    if (xy_changed && !ct_changed) {
        mode = 1;
    } else if (ct_changed && !xy_changed) {
        mode = 2;
    }
    if (mode == dev->state.color_mode) {
        return false;
    }
    dev->state.color_mode = mode;
    ESP_LOGI(TAG, "0x%04x 颜色模式由属性变化推导为 %s", dev->short_addr, mode == 1 ? "XY" : "CT");
    return true;
}

void zb_dev_handle_read_rsp(const ezb_zcl_cmd_read_attr_rsp_message_t *rsp)
{
    int idx;
    zb_dev_t *dev;
    bool changed = false;
    bool xy_changed = false;
    bool ct_changed = false;

    if (rsp == NULL || rsp->in.header == NULL) {
        return;
    }
    idx = dev_find_idx_short(rsp->in.header->src_addr.u.short_addr);
    if (idx < 0) {
        return;
    }
    dev = &s_devs[idx];
    const uint16_t cluster = rsp->in.header->cluster_id;
    const uint8_t old_caps = dev->caps;

    for (const ezb_zcl_read_attr_rsp_variable_t *var = rsp->in.variables; var != NULL; var = var->next) {
        if (var->status != EZB_ZCL_STATUS_SUCCESS) {
            continue;
        }
        update_attr(dev, cluster, var->attr_id, var->attr_value, &changed, &xy_changed,
                    &ct_changed);
    }
    changed |= apply_color_mode(dev, xy_changed, ct_changed);

    if (dev->caps != old_caps) {
        dev_save();
        if (s_discovered_cb != NULL) {
            s_discovered_cb(dev);
        }
    }

    if (changed) {
        ESP_LOGI(TAG, "0x%04x 状态 on=%d level=%d x=%u y=%u mireds=%u mode=%d", dev->short_addr,
                 dev->state.on, dev->state.level, dev->state.x, dev->state.y, dev->state.mireds,
                 dev->state.color_mode);
        notify_state(dev);
    }
}

void zb_dev_handle_report(const ezb_zcl_cmd_report_attr_message_t *msg)
{
    int idx;
    zb_dev_t *dev;
    bool changed = false;
    bool xy_changed = false;
    bool ct_changed = false;

    if (msg == NULL || msg->in.header == NULL) {
        return;
    }
    idx = dev_find_idx_short(msg->in.header->src_addr.u.short_addr);
    if (idx < 0) {
        return;
    }
    dev = &s_devs[idx];

    for (const ezb_zcl_report_attr_variable_t *var = msg->in.variables; var != NULL; var = var->next) {
        update_attr(dev, msg->in.header->cluster_id, var->attr_id, var->attr_value, &changed,
                    &xy_changed, &ct_changed);
    }
    changed |= apply_color_mode(dev, xy_changed, ct_changed);
    if (changed) {
        notify_state(dev);
    }
}

uint8_t zb_dev_count(void)
{
    uint8_t n = 0;

    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (s_devs[i].used) {
            n++;
        }
    }
    return n;
}

const zb_dev_t *zb_dev_at(uint8_t index)
{
    return (index < ZB_DEV_MAX) ? &s_devs[index] : NULL;
}

void zb_dev_dump(void)
{
    char caps_buf[32];

    ESP_LOGI(TAG, "设备表：%d 项", zb_dev_count());
    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (!s_devs[i].used) {
            continue;
        }
        ESP_LOGI(TAG, "[%d] ieee=0x%016llx short=0x%04x ep=%d %s caps=%s on=%d level=%d", i,
                 (unsigned long long)s_devs[i].ieee, s_devs[i].short_addr, s_devs[i].ep,
                 s_devs[i].online ? "online" : "offline",
                 zb_dev_caps_str(s_devs[i].caps, caps_buf, sizeof(caps_buf)), s_devs[i].state.on,
                 s_devs[i].state.level);
    }
}
