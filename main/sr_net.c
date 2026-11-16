#include "sr_net.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "sr_net";

static esp_netif_t *s_sta;
static bool s_wifi_inited;
static bool s_wifi_started;
static bool s_connect_enabled;
static volatile bool s_connected;
static char s_ip[16] = "-";
static int s_rssi;
static bool s_handlers;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_delay_ms = 3000;
static char s_ssid[33];
static char s_pass[65];
static sr_net_event_cb_t s_event_cb;
static bool s_sntp_started;

void sr_net_set_event_cb(sr_net_event_cb_t cb) { s_event_cb = cb; }

static void notify(void) {
    if (s_event_cb) s_event_cb();
}

static void retry_cb(void *arg) {
    (void)arg;
    if (!s_connect_enabled || s_connected) return;
    ESP_LOGI(TAG, "重连 Wi-Fi（下次间隔 %u ms）", (unsigned)(s_retry_delay_ms * 2));
    esp_wifi_connect();
}

static void schedule_retry(void) {
    if (!s_retry_timer) return;
    esp_timer_stop(s_retry_timer);
    const uint32_t delay = s_retry_delay_ms > 60000 ? 60000 : s_retry_delay_ms;
    esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000);
    s_retry_delay_ms = delay < 60000 ? delay * 2 : 60000;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        if (s_connect_enabled) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_rssi = 0;
        strcpy(s_ip, "-");
        ESP_LOGW(TAG, "Wi-Fi 断开 reason=%d", ev ? ev->reason : -1);
        schedule_retry();
        notify();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_connected = true;
    s_retry_delay_ms = 3000;
    ESP_LOGI(TAG, "已连接 IP=%s", s_ip);
    wifi_ap_record_t record;
    if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) s_rssi = record.rssi;

    if (!s_sntp_started) {
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "cn.pool.ntp.org");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        s_sntp_started = true;
    }
    notify();
}

static esp_err_t wifi_common_init(void) {
    if (s_wifi_inited) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_handlers) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
        if (err != ESP_OK) return err;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL);
        if (err != ESP_OK) return err;
        s_handlers = true;
    }
    if (!s_retry_timer) {
        const esp_timer_create_args_t args = {.callback = retry_cb, .name = "sr_wifi_retry"};
        err = esp_timer_create(&args, &s_retry_timer);
        if (err != ESP_OK) return err;
    }
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    s_wifi_inited = true;
    return ESP_OK;
}

esp_err_t sr_net_start(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t err = wifi_common_init();
    if (err != ESP_OK) return err;
    if (!s_sta) {
        s_sta = esp_netif_create_default_wifi_sta();
        if (!s_sta) return ESP_ERR_NO_MEM;
    }
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    strncpy(s_pass, pass ? pass : "", sizeof(s_pass) - 1);

    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, s_ssid, strlen(s_ssid));
    memcpy(wc.sta.password, s_pass, strlen(s_pass));
    // 阈值放到 WPA_PSK：WPA/WPA2 混合、WPA2/WPA3 过渡、WPA3-SAE 的 AP 都能匹配；
    // 之前写死 WPA2_PSK 会把只广播 WPA-PSK 的老路由器判成“无兼容安全方式”（reason 210）。
    // 具体安全方式仍由 AP 与密码协商，不降低实际加密强度。
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    if (!s_wifi_started) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) return err;
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) return err;
        s_connect_enabled = true;
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
    } else {
        s_connect_enabled = true;
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) return err;
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "连接 SSID=%s", s_ssid);
    return ESP_OK;
}

bool sr_net_connected(void) { return s_connected; }

bool sr_net_time_valid(void) {
    const time_t now = time(NULL);
    return now > 1735689600;  // 2025-01-01：证书校验需要可信时间
}

void sr_net_get_status(sr_net_status_t *status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->started = s_wifi_started;
    status->connected = s_connected;
    status->time_valid = sr_net_time_valid();
    snprintf(status->ip, sizeof(status->ip), "%s", s_ip);
    wifi_ap_record_t record;
    if (s_connected && esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
        status->rssi = record.rssi;
        memcpy(status->ssid, record.ssid, sizeof(record.ssid));
        status->ssid[sizeof(status->ssid) - 1] = '\0';
    }
}
