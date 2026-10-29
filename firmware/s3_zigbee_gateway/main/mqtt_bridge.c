#include "mqtt_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "wifi_prov.h"
#include "zb_ctrl.h"

#define TAG "MQTT_BRIDGE"

#define TOPIC_BASE "zbgw"
#define AVAIL_TOPIC TOPIC_BASE "/status"
#define DISCOVERY_PREFIX "homeassistant"
#define SW_VERSION "0.1.0"

#define MQTT_URI_MAX 160
#define ZB_NAME_MAX 48
#define TOPIC_MAX 64

static esp_mqtt_client_handle_t s_client;
static uint64_t s_announced[ZB_DEV_MAX];
static uint32_t s_fingerprint[ZB_DEV_MAX];
static bool s_online;
static char s_gateway_id[20];
static char s_mqtt_uri[MQTT_URI_MAX];

static void publish(const char *topic, const char *payload, int len, bool retain)
{
    if (s_client == NULL) {
        return;
    }
    esp_mqtt_client_publish(s_client, topic, payload, len, 1, retain);
}

static int announced_slot(uint64_t ieee)
{
    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (s_announced[i] == ieee) {
            return i;
        }
    }
    return -1;
}

static uint32_t dev_fingerprint(const zb_dev_t *dev)
{
    return (uint32_t)dev->caps | ((uint32_t)dev->state.min_mireds << 8) |
           ((uint32_t)dev->state.max_mireds << 20);
}

static void mark_announced(uint64_t ieee, uint32_t fingerprint)
{
    for (int i = 0; i < ZB_DEV_MAX; i++) {
        if (s_announced[i] == 0 || s_announced[i] == ieee) {
            s_announced[i] = ieee;
            s_fingerprint[i] = fingerprint;
            return;
        }
    }
}

static void add_color_modes(cJSON *root, uint8_t caps)
{
    cJSON *modes = cJSON_AddArrayToObject(root, "supported_color_modes");

    if ((caps & ZB_CAP_COLOR_XY) && (caps & ZB_CAP_COLOR_TEMP)) {
        cJSON_AddItemToArray(modes, cJSON_CreateString("color_temp"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("xy"));
    } else if (caps & ZB_CAP_COLOR_XY) {
        cJSON_AddItemToArray(modes, cJSON_CreateString("xy"));
    } else if (caps & ZB_CAP_COLOR_TEMP) {
        cJSON_AddItemToArray(modes, cJSON_CreateString("color_temp"));
    } else if (caps & ZB_CAP_LEVEL) {
        cJSON_AddItemToArray(modes, cJSON_CreateString("brightness"));
    } else {
        cJSON_AddItemToArray(modes, cJSON_CreateString("onoff"));
    }
}

static void publish_discovery(const zb_dev_t *dev)
{
    char topic[TOPIC_MAX];
    char name[ZB_NAME_MAX];
    char base[TOPIC_MAX];
    char uniq[ZB_NAME_MAX];

    snprintf(topic, sizeof(topic), DISCOVERY_PREFIX "/light/zbgw_%016llx/config",
             (unsigned long long)dev->ieee);
    snprintf(name, sizeof(name), "Zigbee 灯 %04X", dev->short_addr);
    snprintf(base, sizeof(base), TOPIC_BASE "/light/%016llx", (unsigned long long)dev->ieee);
    snprintf(uniq, sizeof(uniq), "zbgw_%016llx", (unsigned long long)dev->ieee);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "~", base);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "uniq_id", uniq);
    cJSON_AddStringToObject(root, "schema", "json");
    cJSON_AddStringToObject(root, "cmd_t", "~/set");
    cJSON_AddStringToObject(root, "stat_t", "~/state");
    cJSON_AddStringToObject(root, "avty_t", "~/avail");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    cJSON_AddBoolToObject(root, "brightness", (dev->caps & ZB_CAP_LEVEL) != 0);
    cJSON_AddNumberToObject(root, "brightness_scale", 254);
    add_color_modes(root, dev->caps);
    if (dev->caps & ZB_CAP_COLOR_TEMP) {
        cJSON_AddNumberToObject(root, "min_mireds", dev->state.min_mireds);
        cJSON_AddNumberToObject(root, "max_mireds", dev->state.max_mireds);
    }

    cJSON *device = cJSON_AddObjectToObject(root, "dev");
    if (device != NULL) {
        cJSON_AddStringToObject(device, "ids", s_gateway_id);
        cJSON_AddStringToObject(device, "name", "ESP Zigbee Gateway");
        cJSON_AddStringToObject(device, "mf", "Espressif");
        cJSON_AddStringToObject(device, "mdl", "ESP32-S3 + ESP32-H2 双模网关");
    }
    cJSON *origin = cJSON_AddObjectToObject(root, "o");
    if (origin != NULL) {
        cJSON_AddStringToObject(origin, "name", "esp-zigbee-gw");
        cJSON_AddStringToObject(origin, "sw", SW_VERSION);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        publish(topic, json, 0, true);
        ESP_LOGI(TAG, "发布 Discovery: %s", topic);
        cJSON_free(json);
    }
    cJSON_Delete(root);
    mark_announced(dev->ieee, dev_fingerprint(dev));
}

static void publish_state(const zb_dev_t *dev)
{
    char topic[TOPIC_MAX];

    snprintf(topic, sizeof(topic), TOPIC_BASE "/light/%016llx/state",
             (unsigned long long)dev->ieee);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "state", dev->state.on ? "ON" : "OFF");
    if (dev->caps & ZB_CAP_LEVEL) {
        cJSON_AddNumberToObject(root, "brightness", dev->state.level);
    }
    const bool xy_valid = (dev->state.x != 0 || dev->state.y != 0);
    if ((dev->caps & ZB_CAP_COLOR_TEMP) && (dev->state.color_mode == 2 || !xy_valid)) {
        cJSON_AddStringToObject(root, "color_mode", "color_temp");
        cJSON_AddNumberToObject(root, "color_temp", dev->state.mireds);
    } else if (dev->caps & ZB_CAP_COLOR_XY) {
        cJSON *color = cJSON_AddObjectToObject(root, "color");
        cJSON_AddStringToObject(root, "color_mode", "xy");
        if (color != NULL) {
            cJSON_AddNumberToObject(color, "x", dev->state.x / 65535.0);
            cJSON_AddNumberToObject(color, "y", dev->state.y / 65535.0);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        publish(topic, json, 0, true);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

static void publish_availability(const zb_dev_t *dev)
{
    char topic[TOPIC_MAX];

    snprintf(topic, sizeof(topic), TOPIC_BASE "/light/%016llx/avail",
             (unsigned long long)dev->ieee);
    publish(topic, dev->online ? "online" : "offline", 0, true);
}

void mqtt_bridge_on_state(const zb_dev_t *dev)
{
    if (dev == NULL || !dev->used) {
        return;
    }
    const int slot = announced_slot(dev->ieee);
    if (slot < 0 || s_fingerprint[slot] != dev_fingerprint(dev)) {
        publish_discovery(dev);
    }
    publish_availability(dev);
    if (dev->online) {
        publish_state(dev);
    }
}

static void handle_set(const char *topic, int topic_len, const char *data, int data_len)
{
    static const char prefix[] = TOPIC_BASE "/light/";
    char hex[17];

    if (topic_len < (int)(sizeof(prefix) - 1) + 16) {
        return;
    }
    if (strncmp(topic, prefix, sizeof(prefix) - 1) != 0) {
        return;
    }
    memcpy(hex, topic + sizeof(prefix) - 1, 16);
    hex[16] = '\0';

    const zb_dev_t *dev = zb_dev_find_ieee(strtoull(hex, NULL, 16));
    if (dev == NULL) {
        ESP_LOGW(TAG, "未找到设备 ieee=0x%s", hex);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "命令 JSON 解析失败");
        return;
    }

    const cJSON *color = cJSON_GetObjectItem(root, "color");
    if (color != NULL) {
        const cJSON *x = cJSON_GetObjectItem(color, "x");
        const cJSON *y = cJSON_GetObjectItem(color, "y");
        if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
            zb_ctrl_set_color_xy(dev, (uint16_t)(x->valuedouble * 65535.0),
                                 (uint16_t)(y->valuedouble * 65535.0));
        }
    }
    const cJSON *ct = cJSON_GetObjectItem(root, "color_temp");
    if (cJSON_IsNumber(ct)) {
        zb_ctrl_set_color_temp(dev, (uint16_t)ct->valueint);
    }
    const cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        zb_ctrl_set_level(dev, (uint8_t)brightness->valueint);
    }
    const cJSON *state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state)) {
        zb_ctrl_set_onoff(dev, strcmp(state->valuestring, "ON") == 0);
    }
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_online = true;
        ESP_LOGI(TAG, "已连接 broker: %s", s_mqtt_uri);
        publish(AVAIL_TOPIC, "online", 0, true);
        esp_mqtt_client_subscribe(s_client, TOPIC_BASE "/light/+/set", 1);
        esp_mqtt_client_subscribe(s_client, DISCOVERY_PREFIX "/status", 1);
        for (int i = 0; i < ZB_DEV_MAX; i++) {
            if (s_announced[i] != 0) {
                s_announced[i] = 0;
            }
        }
        for (uint8_t i = 0; i < ZB_DEV_MAX; i++) {
            const zb_dev_t *dev = zb_dev_at(i);
            if (dev != NULL && dev->used && dev->online) {
                mqtt_bridge_on_state(dev);
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_online = false;
        ESP_LOGW(TAG, "与 broker 断开，等待重连");
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == (int)strlen(DISCOVERY_PREFIX "/status")) {
            ESP_LOGI(TAG, "HA birth 消息，重发 Discovery");
            for (uint8_t i = 0; i < ZB_DEV_MAX; i++) {
                const zb_dev_t *dev = zb_dev_at(i);
                if (dev != NULL && dev->used && dev->online) {
                    publish_discovery(dev);
                }
            }
            break;
        }
        handle_set(event->topic, event->topic_len, event->data, event->data_len);
        break;
    default:
        break;
    }
}

void mqtt_bridge_start(void)
{
    uint8_t mac[6] = {0};

    if (wifi_prov_get_mqtt_uri(s_mqtt_uri, sizeof(s_mqtt_uri)) != ESP_OK) {
        ESP_LOGW(TAG, "未配置 MQTT 地址，跳过 MQTT（配网页面可填）");
        return;
    }
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        snprintf(s_gateway_id, sizeof(s_gateway_id), "zbgw");
    } else {
        snprintf(s_gateway_id, sizeof(s_gateway_id), "zbgw-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_mqtt_uri,
        .session.last_will = {
            .topic = AVAIL_TOPIC,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt client 初始化失败");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt client 启动失败");
        return;
    }
    ESP_LOGI(TAG, "MQTT 桥启动，broker=%s gateway_id=%s", s_mqtt_uri, s_gateway_id);
}
