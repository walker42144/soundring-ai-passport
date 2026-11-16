// main/sr_store.h
// NVS 配置与授权存储。设备端没有 Keystore；边界见 docs/soundring/README.md。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#define SR_SSID_CAP 33
#define SR_PASS_CAP 65
#define SR_CLIENT_ID_CAP 64
#define SR_TOKEN_CAP 512
#define SR_SCOPE_CAP 200
#define SR_WIFI_MAX 4

typedef struct {
    char ssid[SR_SSID_CAP];
    char pass[SR_PASS_CAP];
} sr_wifi_entry_t;

typedef struct {
    char wifi_ssid[SR_SSID_CAP];
    char wifi_pass[SR_PASS_CAP];
    char client_id[SR_CLIENT_ID_CAP];
    char access_token[SR_TOKEN_CAP];
    char refresh_token[SR_TOKEN_CAP];
    char scope[SR_SCOPE_CAP];
    int64_t expires_at_ms;
    uint8_t brightness;   // 10..100（下限与 apply_brightness_locked 一致）
    uint16_t dim_sec;     // 调暗时间，0=关闭
    uint16_t off_sec;     // 熄屏时间，0=关闭；独立于调暗时间
    uint8_t always_on;    // 常亮只禁用自动熄屏，调暗仍生效
    uint8_t poll_mode;    // 0=省电, 1=标准, 2=快速, 3=自动
} sr_config_t;

esp_err_t sr_store_init(void);
void sr_store_load(sr_config_t *cfg);
esp_err_t sr_store_save_network(const char *ssid, const char *pass);
esp_err_t sr_store_save_auth(const char *client_id, const char *access, const char *refresh,
                             const char *scope, int64_t expires_at_ms);
esp_err_t sr_store_update_tokens(const char *access, const char *refresh, int64_t expires_at_ms);
esp_err_t sr_store_save_settings(const sr_config_t *cfg);
esp_err_t sr_store_clear_auth(void);
esp_err_t sr_store_clear_all(void);
bool sr_store_has_wifi(const sr_config_t *cfg);
bool sr_store_has_auth(const sr_config_t *cfg);
esp_err_t sr_store_add_network(const char *ssid, const char *pass, bool activate);
size_t sr_store_list_networks(sr_wifi_entry_t *out, size_t cap);
