#include "sr_store.h"
#include "sr_policy.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "sr_store";
static const char *k_ns = "soundring";

static nvs_handle_t open_ns(nvs_open_mode_t mode) {
    nvs_handle_t handle = 0;
    const esp_err_t err = nvs_open(k_ns, mode, &handle);
    // 首次开机的空 NVS 没有本命名空间，不是错误。
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return 0;
    }
    return err == ESP_OK ? handle : 0;
}

static void read_str(nvs_handle_t handle, const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    size_t length = cap;
    (void)nvs_get_str(handle, key, out, &length);
}

esp_err_t sr_store_init(void) {
    const nvs_handle_t handle = open_ns(NVS_READWRITE);
    if (!handle) return ESP_FAIL;
    nvs_close(handle);
    return ESP_OK;
}

void sr_store_load(sr_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->brightness = 80;
    cfg->dim_sec = 60;
    // 默认省电：C3 每 3 秒一次完整 TLS 握手对电池设备太贵，需要更顺的进度再手动切换。
    cfg->poll_mode = SR_POLL_ECO;

    const nvs_handle_t handle = open_ns(NVS_READONLY);
    if (!handle) return;
    read_str(handle, "ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    read_str(handle, "wpass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    read_str(handle, "cid", cfg->client_id, sizeof(cfg->client_id));
    read_str(handle, "atok", cfg->access_token, sizeof(cfg->access_token));
    read_str(handle, "rtok", cfg->refresh_token, sizeof(cfg->refresh_token));
    read_str(handle, "scope", cfg->scope, sizeof(cfg->scope));
    (void)nvs_get_i64(handle, "exp", &cfg->expires_at_ms);

    uint8_t brightness = 0;
    // 与 sr_app.c 的 apply_brightness_locked() 一致：可调下限是 10，否则调到 10 后再重启会被丢弃。
    if (nvs_get_u8(handle, "bright", &brightness) == ESP_OK && brightness >= 10 &&
        brightness <= 100) {
        cfg->brightness = brightness;
    }
    uint16_t dim = 0;
    if (nvs_get_u16(handle, "dim", &dim) == ESP_OK && dim <= 600) cfg->dim_sec = dim;
    uint8_t poll = 0;
    if (nvs_get_u8(handle, "poll", &poll) == ESP_OK && poll <= SR_POLL_QUICK) cfg->poll_mode = poll;
    nvs_close(handle);
}

esp_err_t sr_store_save_network(const char *ssid, const char *pass) {
    const nvs_handle_t handle = open_ns(NVS_READWRITE);
    if (!handle) return ESP_FAIL;
    esp_err_t err = nvs_set_str(handle, "ssid", ssid ? ssid : "");
    if (err == ESP_OK) err = nvs_set_str(handle, "wpass", pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t sr_store_save_auth(const char *client_id, const char *access, const char *refresh,
                             const char *scope, int64_t expires_at_ms) {
    if (!access || !refresh) return ESP_ERR_INVALID_ARG;
    const nvs_handle_t handle = open_ns(NVS_READWRITE);
    if (!handle) return ESP_FAIL;
    esp_err_t err = nvs_set_str(handle, "cid", client_id ? client_id : "");
    if (err == ESP_OK) err = nvs_set_str(handle, "atok", access);
    if (err == ESP_OK) err = nvs_set_str(handle, "rtok", refresh);
    if (err == ESP_OK) err = nvs_set_str(handle, "scope", scope ? scope : "");
    if (err == ESP_OK) err = nvs_set_i64(handle, "exp", expires_at_ms);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t sr_store_update_tokens(const char *access, const char *refresh, int64_t expires_at_ms) {
    if (!access) return ESP_ERR_INVALID_ARG;
    const nvs_handle_t handle = open_ns(NVS_READWRITE);
    if (!handle) return ESP_FAIL;
    esp_err_t err = nvs_set_str(handle, "atok", access);
    if (err == ESP_OK && refresh && *refresh) err = nvs_set_str(handle, "rtok", refresh);
    if (err == ESP_OK) err = nvs_set_i64(handle, "exp", expires_at_ms);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t sr_store_save_settings(const sr_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    const nvs_handle_t handle = open_ns(NVS_READWRITE);
    if (!handle) return ESP_FAIL;
    esp_err_t err = nvs_set_u8(handle, "bright", cfg->brightness);
    if (err == ESP_OK) err = nvs_set_u16(handle, "dim", cfg->dim_sec);
    if (err == ESP_OK) err = nvs_set_u8(handle, "poll", cfg->poll_mode <= SR_POLL_QUICK ? cfg->poll_mode : SR_POLL_ECO);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t sr_store_clear_auth(void) {
    const nvs_handle_t handle = open_ns(NVS_READWRITE);
    if (!handle) return ESP_FAIL;
    (void)nvs_erase_key(handle, "cid");
    (void)nvs_erase_key(handle, "atok");
    (void)nvs_erase_key(handle, "rtok");
    (void)nvs_erase_key(handle, "scope");
    (void)nvs_erase_key(handle, "exp");
    const esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t sr_store_clear_all(void) {
    const nvs_handle_t handle = open_ns(NVS_READWRITE);
    if (!handle) return ESP_FAIL;
    const esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) (void)nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool sr_store_has_wifi(const sr_config_t *cfg) { return cfg && cfg->wifi_ssid[0] != '\0'; }
bool sr_store_has_auth(const sr_config_t *cfg) {
    return cfg && cfg->refresh_token[0] != '\0' && cfg->client_id[0] != '\0';
}
