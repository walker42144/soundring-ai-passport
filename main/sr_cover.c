#include "sr_cover.h"
#include "sr_cover_scale.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "jpeg_decoder.h"
#include "sr_text.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "sr_cover";
static const char *k_mount = "/covers";
static const size_t k_cache_bytes = 512 * 1024;
static const int k_cache_files = 8;

// 160x160 RGB565：静态分配，避免与 TLS/JSON 抢堆形成碎片。
static uint16_t s_pixels[SR_COVER_MAX_DIM * SR_COVER_MAX_DIM];
static uint16_t s_width;
static uint16_t s_height;
static bool s_ready;
static char s_url[200];
static bool s_mounted;

esp_err_t sr_cover_init(void) {
    if (s_mounted) return ESP_OK;
    const esp_vfs_spiffs_conf_t config = {
        .base_path = k_mount,
        .partition_label = "covers",
        .max_files = 3,
        .format_if_mount_failed = true,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "封面缓存分区不可用: %s（继续只用内存）", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    return ESP_OK;
}

bool sr_cover_matches(const char *url) { return s_ready && url && strcmp(url, s_url) == 0; }

const uint8_t *sr_cover_pixels(uint16_t *width, uint16_t *height) {
    if (!s_ready) return NULL;
    if (width) *width = s_width;
    if (height) *height = s_height;
    return (const uint8_t *)s_pixels;
}

void sr_cover_clear(void) {
    s_ready = false;
    s_url[0] = '\0';
    s_width = 0;
    s_height = 0;
}

static void cache_path(const char *url, char *out, size_t cap) {
    char name[17];
    snprintf(name, sizeof(name), "%08x%08x", (unsigned)sr_hash32(url),
             (unsigned)sr_hash32(url + strlen(url) / 2));
    snprintf(out, cap, "%s/%s.jpg", k_mount, name);
}
static void cache_trim(void) {
    if (!s_mounted) return;
    DIR *dir = opendir(k_mount);
    if (!dir) return;
    struct dirent *entry;
    size_t total = 0;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%.200s", k_mount, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        total += (size_t)st.st_size;
        ++count;
    }
    closedir(dir);

    while (count > k_cache_files || total > k_cache_bytes) {
        DIR *scan = opendir(k_mount);
        if (!scan) break;
        char oldest[320] = "";
        time_t oldest_time = 0;
        size_t oldest_size = 0;
        while ((entry = readdir(scan)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char path[320];
            snprintf(path, sizeof(path), "%s/%.200s", k_mount, entry->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            if (oldest[0] == '\0' || st.st_mtime < oldest_time) {
                snprintf(oldest, sizeof(oldest), "%s", path);
                oldest_time = st.st_mtime;
                oldest_size = (size_t)st.st_size;
            }
        }
        closedir(scan);
        if (!oldest[0]) break;
        if (unlink(oldest) != 0) break;
        total -= oldest_size;
        --count;
    }
}

static bool decode(const uint8_t *data, size_t length) {
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)data,
        .indata_size = (uint32_t)length,
        .outbuf = (uint8_t *)s_pixels,
        .outbuf_size = sizeof(s_pixels),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "封面尺寸不可读");
        return false;
    }
    const uint16_t longest = info.width > info.height ? info.width : info.height;
    cfg.out_scale = longest > SR_COVER_MAX_DIM * 4   ? JPEG_IMAGE_SCALE_1_8
                    : longest > SR_COVER_MAX_DIM * 2 ? JPEG_IMAGE_SCALE_1_4
                    : longest > SR_COVER_MAX_DIM     ? JPEG_IMAGE_SCALE_1_2
                                                     : JPEG_IMAGE_SCALE_0;
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "封面解码失败");
        return false;
    }
    if (info.output_len > sizeof(s_pixels)) {
        ESP_LOGW(TAG, "封面解码输出超界 %u", (unsigned)info.output_len);
        return false;
    }
    s_width = info.width;
    s_height = info.height;
    if (!sr_cover_upscale_rgb565(s_pixels, &s_width, &s_height, SR_COVER_MAX_DIM,
                                 sizeof(s_pixels) / sizeof(s_pixels[0]))) {
        ESP_LOGW(TAG, "封面尺寸超出缓冲");
        return false;
    }
    s_ready = true;
    return true;
}

static bool load_cached(const char *url) {
    if (!s_mounted) return false;
    char path[64];
    cache_path(url, path, sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > SR_COVER_MAX_BYTES) {
        fclose(file);
        return false;
    }
    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(file);
        return false;
    }
    const size_t read = fread(data, 1, (size_t)size, file);
    fclose(file);
    const bool ok = read == (size_t)size && decode(data, read);
    free(data);
    return ok;
}

static void save_cached(const char *url, const uint8_t *data, size_t length) {
    if (!s_mounted) return;
    char path[64];
    cache_path(url, path, sizeof(path));
    FILE *file = fopen(path, "wb");
    if (!file) return;
    const size_t written = fwrite(data, 1, length, file);
    fclose(file);
    if (written == length) cache_trim();
}

static bool download(const char *url) {
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    const int content_length = esp_http_client_get_content_length(client);
    if (status != 200 || content_length > SR_COVER_MAX_BYTES) {
        ESP_LOGW(TAG, "封面下载被拒绝 status=%d length=%d", status, content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    size_t cap = content_length > 0 ? (size_t)content_length : 2048;
    if (cap > SR_COVER_MAX_BYTES) cap = SR_COVER_MAX_BYTES;
    uint8_t *data = malloc(cap);
    if (!data) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    size_t used = 0;
    bool failed = false;
    for (;;) {
        if (content_length > 0 && used >= (size_t)content_length) break;
        if (used == cap) {
            if (cap == SR_COVER_MAX_BYTES) {
                char extra;
                const int more = esp_http_client_read(client, &extra, 1);
                failed = more != 0;
                break;
            }
            const size_t next = cap > SR_COVER_MAX_BYTES / 2 ? SR_COVER_MAX_BYTES : cap * 2;
            uint8_t *grown = realloc(data, next);
            if (!grown) { failed = true; break; }
            data = grown;
            cap = next;
        }
        const int read = esp_http_client_read(client, (char *)data + used, (int)(cap - used));
        if (read < 0) { failed = true; break; }
        if (read == 0) break;
        used += (size_t)read;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (failed || used == 0 || (content_length > 0 && used != (size_t)content_length)) {
        free(data);
        return false;
    }
    const bool ok = decode(data, used);
    if (ok) save_cached(url, data, used);
    free(data);
    return ok;
}

bool sr_cover_prepare(const char *url) {
    if (!url || strncmp(url, "https://", 8) != 0) {
        sr_cover_clear();
        return false;
    }
    if (sr_cover_matches(url)) return true;
    sr_cover_clear();
    if (load_cached(url)) {
        sr_utf8_ellipsis(url, s_url, sizeof(s_url));
        return true;
    }
    if (download(url)) {
        sr_utf8_ellipsis(url, s_url, sizeof(s_url));
        return true;
    }
    return false;
}
