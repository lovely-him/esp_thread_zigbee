#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "WIFI_PROV"

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_MQTT_URI "mqtt_uri"

#define AP_SSID_PREFIX "ESP-ZB-GW-"
#define STA_GOT_IP_BIT BIT0
#define STA_FAIL_BIT BIT1
#define STA_MAX_RETRY 10
#define STA_CONNECT_TIMEOUT_MS 20000

static EventGroupHandle_t s_events;
static httpd_handle_t s_server;
static char s_sta_ip[16];
static char s_ssid[33];
static char s_pass[65];
static int s_retry;

static const char *k_form_html =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Zigbee 网关配网</title></head><body>"
    "<h2>ESP Zigbee 网关 - Wi-Fi 配网</h2>"
    "<p><b>仅支持 2.4GHz Wi-Fi</b>（不支持 5GHz）。"
    "若路由器 2.4G/5G 同名，请改填 2.4G 专属 SSID。</p>"
    "<form method=\"POST\" action=\"/save\">"
    "<p>Wi-Fi 名称 (SSID)<br><input name=\"ssid\" maxlength=\"32\" size=\"40\"></p>"
    "<p>密码<br><input name=\"pass\" type=\"password\" maxlength=\"64\" size=\"40\"></p>"
    "<p>MQTT 地址（可留空，稍后再配）<br>"
    "<input name=\"mqtt\" maxlength=\"96\" size=\"40\" "
    "placeholder=\"mqtt://user:pass@192.168.63.179:1883\"></p>"
    "<p><input type=\"submit\" value=\"保存并重启\"></p>"
    "</form></body></html>";

static esp_err_t nvs_load_str(const char *key, char *out, size_t len)
{
    nvs_handle_t handle;
    size_t size = len;

    out[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = nvs_get_str(handle, key, out, &size);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_load_credentials(void)
{
    nvs_handle_t handle;
    size_t len = sizeof(s_ssid);
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, NVS_KEY_SSID, s_ssid, &len);
    if (err == ESP_OK) {
        len = sizeof(s_pass);
        if (nvs_get_str(handle, NVS_KEY_PASS, s_pass, &len) != ESP_OK) {
            s_pass[0] = '\0';
        }
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_save_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        if (s_retry < STA_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "STA 断开 reason=%d，重连 %d/%d", ev->reason, s_retry, STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "STA 断开 reason=%d，已达最大重试次数", ev->reason);
            xEventGroupSetBits(s_events, STA_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, STA_GOT_IP_BIT);
    }
}

static esp_err_t wifi_start_sta(void)
{
    wifi_config_t cfg = {0};

    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set sta mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set sta config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    return ESP_OK;
}

static void url_decode(char *dst, const char *src)
{
    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            const char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (src[0] == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool form_field(const char *body, const char *name, char *out, size_t out_size)
{
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);

    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if (p == body || p[-1] == '&') {
            p += strlen(key);
            const char *end = strchr(p, '&');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            char raw[160];
            if (len >= sizeof(raw)) {
                len = sizeof(raw) - 1;
            }
            memcpy(raw, p, len);
            raw[len] = '\0';
            url_decode(out, raw);
            (void)out_size;
            return true;
        }
        p += strlen(key);
    }
    return false;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_form_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[384];
    char ssid[33] = {0};
    char pass[65] = {0};
    char mqtt[97] = {0};
    const int len = req->content_len;

    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }
    if (httpd_req_recv(req, body, len) != len) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[len] = '\0';

    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "mqtt", mqtt, sizeof(mqtt));

    if (ssid[0] == '\0') {
        if (nvs_load_str(NVS_KEY_SSID, ssid, sizeof(ssid)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
            return ESP_FAIL;
        }
        if (nvs_load_str(NVS_KEY_PASS, pass, sizeof(pass)) != ESP_OK) {
            pass[0] = '\0';
        }
    }

    if (nvs_save_credentials(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
        return ESP_FAIL;
    }
    if (mqtt[0] != '\0') {
        if (nvs_save_str(NVS_KEY_MQTT_URI, mqtt) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "已保存凭据 SSID=%s MQTT=%s，重启生效", ssid, mqtt[0] ? mqtt : "(沿用旧值)");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>已保存，设备正在重启…</h3></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;

    if (s_server != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "http server start failed");

    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root), TAG, "register / failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &save), TAG, "register /save failed");
    return ESP_OK;
}

static esp_err_t wifi_start_ap(void)
{
    uint8_t mac[6] = {0};
    wifi_config_t cfg = {0};

    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "%s%02X%02X%02X", AP_SSID_PREFIX, mac[3],
             mac[4], mac[5]);
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set ap mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "set ap config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "ap start failed");

    ESP_LOGW(TAG, "进入 SoftAP 配网：SSID=%s，浏览器打开 http://192.168.4.1/", cfg.ap.ssid);
    return start_http_server();
}

esp_err_t wifi_prov_get_mqtt_uri(char *out, size_t len)
{
    nvs_handle_t handle;
    size_t size = len;

    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = nvs_get_str(handle, NVS_KEY_MQTT_URI, out, &size);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_prov_connect(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "event group create failed");

    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_FAIL, TAG,
                        "create sta netif failed");

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "register wifi event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "register ip event failed");

    if (nvs_load_credentials() == ESP_OK) {
        ESP_LOGI(TAG, "尝试连接 Wi-Fi: %s", s_ssid);
        ESP_RETURN_ON_ERROR(wifi_start_sta(), TAG, "sta start failed");
        const EventBits_t bits =
            xEventGroupWaitBits(s_events, STA_GOT_IP_BIT | STA_FAIL_BIT, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
        if (bits & STA_GOT_IP_BIT) {
            ESP_LOGI(TAG, "Wi-Fi 已连接：%s（浏览器打开 http://%s/ 可修改 MQTT）", s_sta_ip, s_sta_ip);
            start_http_server();
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Wi-Fi 连接失败（已重试 %d 次），转入配网模式", s_retry);
        esp_wifi_stop();
    } else {
        ESP_LOGW(TAG, "NVS 中无 Wi-Fi 凭据，转入配网模式");
    }

    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap() != NULL, ESP_FAIL, TAG,
                        "create ap netif failed");
    ESP_RETURN_ON_ERROR(wifi_start_ap(), TAG, "ap mode failed");
    return WIFI_PROV_AP_MODE;
}
