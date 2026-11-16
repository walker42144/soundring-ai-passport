#include "sr_spotify.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sr_net.h"
#include "sr_policy.h"
#include "sr_text.h"
#include "sr_queue_stream.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "sr_sp";
static const char *k_api = "https://api.spotify.com/v1";
static const char *k_token_url = "https://accounts.spotify.com/api/token";

void sr_sp_init(sr_spotify_t *sp, sr_config_t *cfg) {
    if (!sp) return;
    memset(sp, 0, sizeof(*sp));
    sp->cfg = cfg;
}

bool sr_sp_blocked(const sr_spotify_t *sp, int64_t now_us) {
    return sp && now_us < sp->blocked_until_us;
}

int64_t sr_sp_block_left_ms(const sr_spotify_t *sp, int64_t now_us) {
    if (!sp || now_us >= sp->blocked_until_us) return 0;
    return (sp->blocked_until_us - now_us) / 1000;
}

void sr_sp_clear_block(sr_spotify_t *sp) {
    if (!sp) return;
    sp->blocked_until_us = 0;
    sp->blocked_quota = false;
}

// 记录一次 429：区分配额用尽与临时限流，并写入用户可读文案。
static void set_result(sr_result_t *result, int status, const char *text) {
    if (!result) return;
    result->status = status;
    sr_utf8_ellipsis(text ? text : "", result->text, sizeof(result->text));
}

static void block_from_429(sr_spotify_t *sp, const sr_http_resp_t *resp, sr_result_t *result) {
    const bool quota = sr_429_quota(resp->body);
    const int64_t wait_ms = sr_429_block_ms(resp->body, resp->retry_after_ms);
    sp->blocked_until_us = esp_timer_get_time() + wait_ms * 1000;
    sp->blocked_quota = quota;
    set_result(result, 429, sr_429_text(quota));
    ESP_LOGW(TAG, "429 %s，退避 %" PRId64 " ms", quota ? "配额用尽" : "临时限流", wait_ms);
}

static const char *reason_from_body(const char *body) {
    if (!body || !*body) return NULL;
    cJSON *root = cJSON_Parse(body);
    if (!root) return NULL;
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *reason = cJSON_IsObject(error) ? cJSON_GetObjectItemCaseSensitive(error, "reason") : NULL;
    static char buffer[48];
    buffer[0] = '\0';
    if (cJSON_IsString(reason) && reason->valuestring) {
        snprintf(buffer, sizeof(buffer), "%s", reason->valuestring);
    }
    cJSON_Delete(root);
    return buffer[0] ? buffer : NULL;
}

static void url_encode(const char *src, char *dst, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t used = 0;
    if (!dst || cap == 0) return;
    for (const unsigned char *p = (const unsigned char *)src; p && *p; ++p) {
        const bool safe = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (safe && used + 1 < cap) {
            dst[used++] = (char)*p;
        } else if (used + 3 < cap) {
            dst[used++] = '%';
            dst[used++] = hex[*p >> 4];
            dst[used++] = hex[*p & 0x0F];
        } else {
            break;
        }
    }
    dst[used] = '\0';
}

bool sr_sp_token_expiring(const sr_spotify_t *sp, int64_t now_epoch_ms) {
    if (!sp || !sp->cfg) return true;
    return sp->cfg->expires_at_ms <= now_epoch_ms + 60000;
}

bool sr_sp_refresh_tokens(sr_spotify_t *sp, sr_result_t *result) {
    if (!sp || !sp->cfg) return false;
    sr_config_t *cfg = sp->cfg;
    if (cfg->refresh_token[0] == '\0' || cfg->client_id[0] == '\0') {
        set_result(result, 401, "尚未完成授权，请在设置中配对");
        return false;
    }

    char refresh[SR_TOKEN_CAP * 2];
    char client[SR_CLIENT_ID_CAP * 2];
    url_encode(cfg->refresh_token, refresh, sizeof(refresh));
    url_encode(cfg->client_id, client, sizeof(client));
    char form[SR_TOKEN_CAP * 2 + SR_CLIENT_ID_CAP * 2 + 80];
    snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s",
             refresh, client);

    sr_http_resp_t resp;
    const esp_err_t err = sr_http_do(SR_M_POST, k_token_url, NULL,
                                     "application/x-www-form-urlencoded", form, &resp, 4096);
    if (err != ESP_OK) {
        set_result(result, 0, err == ESP_ERR_NO_MEM ? "设备内存不足，请稍后重试" : "连接授权服务失败，请检查网络");
        return false;
    }
    if (resp.status == 429) {
        // 配额用尽与临时限流都是 429，但恢复时间差两个数量级，不能一概而论。
        block_from_429(sp, &resp, result);
        sr_http_resp_clear(&resp);
        return false;
    }
    if (resp.status != 200 || !resp.body) {
        const bool revoked = resp.status == 400 || resp.status == 401;
        set_result(result, resp.status, revoked ? "授权失效，请重新配对" : "授权服务异常，请稍后重试");
        if (revoked) sp->auth_failed = true;
        sr_http_resp_clear(&resp);
        return false;
    }

    cJSON *root = cJSON_Parse(resp.body);
    const cJSON *access = root ? cJSON_GetObjectItemCaseSensitive(root, "access_token") : NULL;
    const cJSON *expires = root ? cJSON_GetObjectItemCaseSensitive(root, "expires_in") : NULL;
    const cJSON *new_refresh = root ? cJSON_GetObjectItemCaseSensitive(root, "refresh_token") : NULL;
    const cJSON *scope = root ? cJSON_GetObjectItemCaseSensitive(root, "scope") : NULL;
    if (!cJSON_IsString(access) || !access->valuestring || !access->valuestring[0] ||
        strlen(access->valuestring) >= sizeof(cfg->access_token) ||
        (cJSON_IsString(new_refresh) && new_refresh->valuestring &&
         strlen(new_refresh->valuestring) >= sizeof(cfg->refresh_token))) {
        set_result(result, 401, "授权服务返回异常，请稍后重试");
        if (root) cJSON_Delete(root);
        sr_http_resp_clear(&resp);
        return false;
    }
    const int64_t ttl = cJSON_IsNumber(expires) ? (int64_t)expires->valueint : 3600;
    const int64_t expires_at = (int64_t)time(NULL) * 1000 + ttl * 1000 - 30000;
    sr_utf8_ellipsis(access->valuestring, cfg->access_token, sizeof(cfg->access_token));
    if (cJSON_IsString(new_refresh) && new_refresh->valuestring) {
        sr_utf8_ellipsis(new_refresh->valuestring, cfg->refresh_token, sizeof(cfg->refresh_token));
    }
    if (cJSON_IsString(scope) && scope->valuestring) {
        sr_utf8_ellipsis(scope->valuestring, cfg->scope, sizeof(cfg->scope));
    }
    cfg->expires_at_ms = expires_at;
    (void)sr_store_update_tokens(cfg->access_token, cfg->refresh_token, expires_at);
    sp->auth_failed = false;
    if (root) cJSON_Delete(root);
    sr_http_resp_clear(&resp);
    ESP_LOGI(TAG, "令牌已刷新，有效期 %" PRId64 " 秒", ttl);
    return true;
}

// 统一请求：阻塞期检查、到期刷新、401 重试一次、429 记录。
static bool sp_call_impl(sr_spotify_t *sp, sr_method_t method, const char *path, const char *body,
                    sr_http_resp_t *resp, size_t max_body, const char *kind, sr_result_t *result,
                    sr_http_chunk_fn consume, void *context) {
    memset(resp, 0, sizeof(*resp));
    const int64_t now_us = esp_timer_get_time();
    if (!sr_net_time_valid()) {
        // 没有可信时间时 TLS 证书校验必然失败，直接给出可读原因，避免误判为 Wi-Fi 故障。
        set_result(result, 0, "正在校时，请稍候");
        return false;
    }
    if (sr_sp_blocked(sp, now_us)) {
        set_result(result, 429, sr_429_text(sp->blocked_quota));
        return false;
    }
    if (sr_sp_token_expiring(sp, (int64_t)time(NULL) * 1000)) {
        if (!sr_sp_refresh_tokens(sp, result)) return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", k_api, path);
    char bearer[SR_TOKEN_CAP + 16];
    snprintf(bearer, sizeof(bearer), "Bearer %s", sp->cfg->access_token);

    esp_err_t err = consume ? sr_http_stream_get(url, bearer, resp, consume, context)
                            : sr_http_do(method, url, bearer, NULL, body, resp, max_body);
    if (err != ESP_OK) {
        set_result(result, 0, err == ESP_ERR_NO_MEM ? "设备内存不足，请稍后重试" : "连接服务失败，请检查网络");
        return false;
    }

    if (resp->status == 401) {
        sr_http_resp_clear(resp);
        if (!sr_sp_refresh_tokens(sp, result)) return false;
        snprintf(bearer, sizeof(bearer), "Bearer %s", sp->cfg->access_token);
        err = consume ? sr_http_stream_get(url, bearer, resp, consume, context)
                      : sr_http_do(method, url, bearer, NULL, body, resp, max_body);
        if (err != ESP_OK) {
            set_result(result, 0, err == ESP_ERR_NO_MEM ? "设备内存不足，请稍后重试" : "连接服务失败，请检查网络");
            return false;
        }
    }

    if (resp->status == 429) {
        block_from_429(sp, resp, result);
        return false;
    }
    if (resp->status < 200 || resp->status >= 300) {
        const char *reason = reason_from_body(resp->body);
        const sr_err_kind_t err_kind = kind[0] == 'r' ? SR_ERR_READ
                                       : kind[0] == 't' ? SR_ERR_TOKEN
                                                        : SR_ERR_CONTROL;
        set_result(result, resp->status, sr_error_text(resp->status, reason, err_kind));
        if (resp->status == 401) sp->auth_failed = true;
        ESP_LOGW(TAG, "%s %s -> %d%s%s", method == SR_M_GET ? "GET" : "SEND", path, resp->status,
                 reason ? " reason=" : "", reason ? reason : "");
        return false;
    }
    if (resp->truncated) {
        set_result(result, 0, "数据过大，暂无法显示");
        return false;
    }
    // 成功即说明限流期已过：顺手清掉封禁时刻与封禁类型，别让 blocked_quota 长期留着。
    sr_sp_clear_block(sp);
    sp->auth_failed = false;
    set_result(result, 1, "已完成");
    return true;
}

static bool sp_call(sr_spotify_t *sp, sr_method_t method, const char *path, const char *body,
                    sr_http_resp_t *resp, size_t max_body, const char *kind, sr_result_t *result) {
    return sp_call_impl(sp, method, path, body, resp, max_body, kind, result, NULL, NULL);
}

static void copy_json_string(const cJSON *object, const char *key, char *dst, size_t cap) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring) {
        sr_utf8_ellipsis(item->valuestring, dst, cap);
    } else {
        dst[0] = '\0';
    }
}

static void join_artists(const cJSON *item, char *dst, size_t cap) {
    dst[0] = '\0';
    const cJSON *artists = cJSON_GetObjectItemCaseSensitive(item, "artists");
    if (!cJSON_IsArray(artists)) return;
    char joined[SR_LIST_SUB_CAP * 2] = "";
    size_t used = 0;
    const cJSON *artist = NULL;
    cJSON_ArrayForEach(artist, artists) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(artist, "name");
        if (!cJSON_IsString(name) || !name->valuestring) continue;
        const size_t length = strlen(name->valuestring);
        if (used && used + 3 < sizeof(joined)) {
            memcpy(joined + used, " / ", 3);
            used += 3;
        }
        if (used + length + 1 >= sizeof(joined)) break;
        memcpy(joined + used, name->valuestring, length);
        used += length;
        joined[used] = '\0';
    }
    sr_utf8_ellipsis(joined, dst, cap);
}

static void parse_track(const cJSON *track, const char *context_uri, int position, sr_item_t *out) {
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(track)) return;
    copy_json_string(track, "name", out->title, sizeof(out->title));
    copy_json_string(track, "uri", out->uri, sizeof(out->uri));
    join_artists(track, out->subtitle, sizeof(out->subtitle));
    if (context_uri) sr_utf8_ellipsis(context_uri, out->context_uri, sizeof(out->context_uri));
    out->position = position;
    const cJSON *playable = cJSON_GetObjectItemCaseSensitive(track, "is_playable");
    out->actionable = out->uri[0] && (!cJSON_IsBool(playable) || cJSON_IsTrue(playable));
}

// 分页只保留 UI 真正用到的 total/offset（页数由 total 推算）；has_more 无使用者，已删除。
static void parse_paging(sr_list_t *list, const cJSON *root) {
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total");
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
    list->total = cJSON_IsNumber(total) ? total->valueint : list->count;
    list->offset = cJSON_IsNumber(offset) ? offset->valueint : 0;
}

bool sr_sp_load_playback(sr_spotify_t *sp, sr_playback_t *out, sr_result_t *result) {
    sr_http_resp_t resp;
    if (!sp_call(sp, SR_M_GET, "/me/player", NULL, &resp, 4096, "read", result)) {
        sr_http_resp_clear(&resp);
        return false;
    }
    if (resp.status == 204 || resp.len == 0) {
        sr_playback_reset(out);
        sr_http_resp_clear(&resp);
        return true;
    }
    const bool ok = sr_playback_parse(resp.body, out);
    sr_http_resp_clear(&resp);
    if (!ok) {
        set_result(result, 1, "Spotify 返回数据异常，稍后重试");
        return false;
    }
    return true;
}

bool sr_sp_load_devices(sr_spotify_t *sp, sr_list_t *out, sr_result_t *result) {
    memset(out, 0, sizeof(*out));
    sr_http_resp_t resp;
    if (!sp_call(sp, SR_M_GET, "/me/player/devices", NULL, &resp, 8192, "read", result)) {
        sr_http_resp_clear(&resp);
        return false;
    }
    cJSON *root = cJSON_Parse(resp.body);
    const cJSON *devices = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    if (!cJSON_IsArray(devices)) {
        set_result(result, 1, "设备列表不可用");
        if (root) cJSON_Delete(root);
        sr_http_resp_clear(&resp);
        return false;
    }
    const cJSON *device = NULL;
    cJSON_ArrayForEach(device, devices) {
        if (out->count >= SR_LIST_MAX) break;
        sr_item_t *item = &out->items[out->count];
        memset(item, 0, sizeof(*item));
        copy_json_string(device, "name", item->title, sizeof(item->title));
        copy_json_string(device, "id", item->uri, sizeof(item->uri));
        const char *type = NULL;
        const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(device, "type");
        if (cJSON_IsString(type_item)) type = type_item->valuestring;
        char type_text[16];
        sr_utf8_ellipsis(type ? type : "设备", type_text, sizeof(type_text));
        const cJSON *active = cJSON_GetObjectItemCaseSensitive(device, "is_active");
        const bool is_active = cJSON_IsTrue(active);
        const cJSON *volume = cJSON_GetObjectItemCaseSensitive(device, "volume_percent");
        const int volume_value = cJSON_IsNumber(volume) ? volume->valueint : -1;
        if (volume_value >= 0) {
            snprintf(item->subtitle, sizeof(item->subtitle), "%s · %d%%%s",
                     type_text, volume_value, is_active ? " · 当前" : "");
        } else {
            snprintf(item->subtitle, sizeof(item->subtitle), "%s%s", type_text,
                     is_active ? " · 当前" : "");
        }
        item->actionable = item->uri[0] && !is_active;
        ++out->count;
    }
    out->total = out->count;
    if (out->count == 0) set_result(result, 1, "没有可用设备，请先打开 Spotify");
    if (root) cJSON_Delete(root);
    sr_http_resp_clear(&resp);
    return true;
}

bool sr_sp_load_playlists(sr_spotify_t *sp, int offset, int limit, sr_list_t *out,
                          sr_result_t *result) {
    memset(out, 0, sizeof(*out));
    if (limit <= 0 || limit > 20) limit = 20;
    char path[96];
    snprintf(path, sizeof(path), "/me/playlists?limit=%d&offset=%d", limit, offset);
    sr_http_resp_t resp;
    if (!sp_call(sp, SR_M_GET, path, NULL, &resp, 16384, "read", result)) {
        sr_http_resp_clear(&resp);
        return false;
    }
    cJSON *root = cJSON_Parse(resp.body);
    const cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "items") : NULL;
    if (!cJSON_IsArray(items)) {
        set_result(result, 1, "歌单列表不可用");
        if (root) cJSON_Delete(root);
        sr_http_resp_clear(&resp);
        return false;
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, items) {
        if (out->count >= SR_LIST_MAX) break;
        sr_item_t *item = &out->items[out->count];
        memset(item, 0, sizeof(*item));
        copy_json_string(entry, "name", item->title, sizeof(item->title));
        copy_json_string(entry, "uri", item->uri, sizeof(item->uri));
        copy_json_string(entry, "id", item->context_uri, sizeof(item->context_uri));
        const cJSON *owner = cJSON_GetObjectItemCaseSensitive(entry, "owner");
        const cJSON *owner_name = owner ? cJSON_GetObjectItemCaseSensitive(owner, "display_name") : NULL;
        const cJSON *count = cJSON_GetObjectItemCaseSensitive(entry, "items");
        if (!cJSON_IsObject(count)) count = cJSON_GetObjectItemCaseSensitive(entry, "tracks");
        const cJSON *total = count ? cJSON_GetObjectItemCaseSensitive(count, "total") : NULL;
        const int tracks = cJSON_IsNumber(total) ? total->valueint : -1;
        char owner_text[SR_LIST_SUB_CAP] = "";
        if (cJSON_IsString(owner_name) && owner_name->valuestring) {
            sr_utf8_ellipsis(owner_name->valuestring, owner_text, sizeof(owner_text));
        }
        char subtitle[128];
        if (tracks >= 0) {
            snprintf(subtitle, sizeof(subtitle), "%s · %d 首",
                     owner_text[0] ? owner_text : "歌单", tracks);
        } else {
            snprintf(subtitle, sizeof(subtitle), "%s", owner_text[0] ? owner_text : "歌单");
        }
        sr_utf8_ellipsis(subtitle, item->subtitle, sizeof(item->subtitle));
        item->actionable = item->uri[0] && sr_playlist_id_valid(item->context_uri);
        item->position = -1;
        ++out->count;
    }
    parse_paging(out, root);
    if (out->count == 0) set_result(result, 1, "没有可读歌单；开发模式只支持本人或协作歌单");
    if (root) cJSON_Delete(root);
    sr_http_resp_clear(&resp);
    return true;
}

bool sr_sp_load_playlist_items(sr_spotify_t *sp, const char *playlist_id, const char *playlist_uri,
                               int offset, int limit, sr_list_t *out, sr_result_t *result) {
    memset(out, 0, sizeof(*out));
    if (!sr_playlist_id_valid(playlist_id)) {
        set_result(result, 1, "歌单不可用");
        return false;
    }
    if (limit <= 0 || limit > 20) limit = 20;
    // 2026-02 起 /tracks 更名为 /items；老账号仍可能返回旧结构，解析同时兼容。
    char path[192];
    snprintf(path, sizeof(path), "/playlists/%s/items?limit=%d&offset=%d&fields=total,offset,items(item(name,uri,is_playable,artists(name)))", playlist_id, limit, offset);
    sr_http_resp_t resp;
    bool ok = sp_call(sp, SR_M_GET, path, NULL, &resp, 16384, "read", result);
    if (!ok && resp.status == 404) {
        sr_http_resp_clear(&resp);
        snprintf(path, sizeof(path), "/playlists/%s/tracks?limit=%d&offset=%d&fields=total,offset,items(track(name,uri,is_playable,artists(name)))", playlist_id, limit, offset);
        ok = sp_call(sp, SR_M_GET, path, NULL, &resp, 16384, "read", result);
    }
    if (!ok) {
        sr_http_resp_clear(&resp);
        return false;
    }
    cJSON *root = cJSON_Parse(resp.body);
    const cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "items") : NULL;
    if (!cJSON_IsArray(items)) {
        set_result(result, 1, "该歌单不返回曲目（仅本人或协作歌单可读）");
        if (root) cJSON_Delete(root);
        sr_http_resp_clear(&resp);
        return false;
    }
    const int base = cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "offset"))
                         ? cJSON_GetObjectItemCaseSensitive(root, "offset")->valueint
                         : offset;
    int index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, items) {
        if (out->count >= SR_LIST_MAX) break;
        const cJSON *track = cJSON_GetObjectItemCaseSensitive(entry, "item");
        if (!cJSON_IsObject(track)) track = cJSON_GetObjectItemCaseSensitive(entry, "track");
        sr_item_t *item = &out->items[out->count];
        parse_track(track, playlist_uri, base + index, item);
        if (item->title[0] == '\0') {
            snprintf(item->title, sizeof(item->title), "不可显示的内容");
            item->actionable = false;
        }
        ++out->count;
        ++index;
    }
    parse_paging(out, root);
    if (out->count == 0) set_result(result, 1, "此页没有曲目");
    if (root) cJSON_Delete(root);
    sr_http_resp_clear(&resp);
    return true;
}

bool sr_sp_load_queue(sr_spotify_t *sp, sr_list_t *out, sr_result_t *result) {
    sr_queue_stream_t reader;
    sr_queue_stream_init(&reader, out);
    sr_http_resp_t resp;
    if (!sp_call_impl(sp, SR_M_GET, "/me/player/queue", NULL, &resp, 0, "read", result,
                      sr_queue_stream_feed, &reader)) {
        if (reader.failed) set_result(result, 0, "队列数据异常，请稍后重试");
        sr_http_resp_clear(&resp);
        return false;
    }
    if (!sr_queue_stream_finish(&reader)) {
        set_result(result, 0, "队列数据异常，请稍后重试");
        sr_http_resp_clear(&resp);
        return false;
    }
    if (!out->count) set_result(result, 1, "队列为空");
    sr_http_resp_clear(&resp);
    return true;
}

bool sr_sp_load_recent(sr_spotify_t *sp, sr_list_t *out, sr_result_t *result) {
    sr_queue_stream_t reader;
    sr_recent_stream_init(&reader, out);
    sr_http_resp_t resp;
    if (!sp_call_impl(sp, SR_M_GET, "/me/player/recently-played?limit=13", NULL,
                      &resp, 0, "read", result, sr_queue_stream_feed, &reader)) {
        if (reader.failed) set_result(result, 0, "最近播放数据异常，请稍后重试");
        sr_http_resp_clear(&resp);
        return false;
    }
    if (!sr_queue_stream_finish(&reader)) {
        set_result(result, 0, "最近播放数据异常，请稍后重试");
        sr_http_resp_clear(&resp);
        return false;
    }
    if (!out->count) set_result(result, 1, "没有最近播放记录");
    sr_http_resp_clear(&resp);
    return true;
}

bool sr_sp_liked(sr_spotify_t *sp, const char *uri, bool *liked, sr_result_t *result) {
    if (!uri || strncmp(uri, "spotify:track:", 14) != 0) {
        set_result(result, 1, "当前内容不支持收藏");
        return false;
    }
    char encoded[SR_URI_CAP * 3];
    url_encode(uri, encoded, sizeof(encoded));
    char path[SR_URI_CAP * 3 + 40];
    snprintf(path, sizeof(path), "/me/library/contains?uris=%s", encoded);
    sr_http_resp_t resp;
    if (!sp_call(sp, SR_M_GET, path, NULL, &resp, 1024, "read", result)) {
        sr_http_resp_clear(&resp);
        return false;
    }
    cJSON *root = cJSON_Parse(resp.body);
    if (!cJSON_IsArray(root)) {
        set_result(result, 1, "收藏状态不可用");
        if (root) cJSON_Delete(root);
        sr_http_resp_clear(&resp);
        return false;
    }
    *liked = cJSON_IsTrue(cJSON_GetArrayItem(root, 0));
    cJSON_Delete(root);
    sr_http_resp_clear(&resp);
    return true;
}

bool sr_sp_set_like(sr_spotify_t *sp, const char *uri, bool liked, sr_result_t *result) {
    if (!uri || strncmp(uri, "spotify:track:", 14) != 0) {
        set_result(result, 1, "当前内容不支持收藏");
        return false;
    }
    // 官方参考文档：uris 是 query 参数（单次最多 40 个），PUT 成功返回 200 且无响应体。
    // 传 body 会被当作无效请求，所以这里一律不带 Content-Type / body。
    char encoded[SR_URI_CAP * 3];
    url_encode(uri, encoded, sizeof(encoded));
    char path[SR_URI_CAP * 3 + 40];
    snprintf(path, sizeof(path), "/me/library?uris=%s", encoded);
    sr_http_resp_t resp;
    const bool ok = sp_call(sp, liked ? SR_M_PUT : SR_M_DELETE, path, NULL, &resp, 0, "control",
                            result);
    sr_http_resp_clear(&resp);
    if (ok) set_result(result, 1, liked ? "已收藏" : "已取消收藏");
    return ok;
}

static bool post_empty(sr_spotify_t *sp, const char *path, sr_result_t *result) {
    sr_http_resp_t resp;
    const bool ok = sp_call(sp, SR_M_POST, path, NULL, &resp, 0, "control", result);
    sr_http_resp_clear(&resp);
    return ok;
}

static bool put_empty(sr_spotify_t *sp, const char *path, sr_result_t *result) {
    sr_http_resp_t resp;
    const bool ok = sp_call(sp, SR_M_PUT, path, "", &resp, 0, "control", result);
    sr_http_resp_clear(&resp);
    return ok;
}

bool sr_sp_command(sr_spotify_t *sp, const sr_command_t *command, sr_result_t *result) {
    if (!sp || !command) return false;
    char path[320];
    char device_query[256] = "";
    if (command->device_id[0]) {
        char encoded[192];
        url_encode(command->device_id, encoded, sizeof(encoded));
        snprintf(device_query, sizeof(device_query), "device_id=%s", encoded);
    }

    switch (command->type) {
        case SR_CMD_PLAY_PAUSE:
            snprintf(path, sizeof(path), "/me/player/%s%s%s",
                     command->value ? "play" : "pause",
                     device_query[0] ? "?" : "", device_query);
            return put_empty(sp, path, result);
        case SR_CMD_NEXT:
        case SR_CMD_PREV: {
            snprintf(path, sizeof(path), "/me/player/%s%s%s",
                     command->type == SR_CMD_NEXT ? "next" : "previous",
                     device_query[0] ? "?" : "", device_query);
            return post_empty(sp, path, result);
        }
        case SR_CMD_VOLUME:
            snprintf(path, sizeof(path), "/me/player/volume?volume_percent=%d%s%s",
                     command->value < 0 ? 0 : (command->value > 100 ? 100 : command->value),
                     device_query[0] ? "&" : "", device_query);
            return put_empty(sp, path, result);
        case SR_CMD_SEEK:
            snprintf(path, sizeof(path), "/me/player/seek?position_ms=%d%s%s",
                     command->value < 0 ? 0 : command->value,
                     device_query[0] ? "&" : "", device_query);
            return put_empty(sp, path, result);
        case SR_CMD_SHUFFLE:
            snprintf(path, sizeof(path), "/me/player/shuffle?state=%s%s%s",
                     command->value ? "true" : "false",
                     device_query[0] ? "&" : "", device_query);
            return put_empty(sp, path, result);
        case SR_CMD_REPEAT: {
            static const char *const modes[] = {"off", "context", "track"};
            const int index = command->value < 0 ? 0 : (command->value > 2 ? 2 : command->value);
            snprintf(path, sizeof(path), "/me/player/repeat?state=%s%s%s", modes[index],
                     device_query[0] ? "&" : "", device_query);
            return put_empty(sp, path, result);
        }
        case SR_CMD_TRANSFER: {
            cJSON *root = cJSON_CreateObject();
            cJSON *ids = cJSON_AddArrayToObject(root, "device_ids");
            cJSON_AddItemToArray(ids, cJSON_CreateString(command->device_id));
            cJSON_AddBoolToObject(root, "play", false);
            char *body = cJSON_PrintUnformatted(root);
            sr_http_resp_t resp = {0};
            const bool ok = body && sp_call(sp, SR_M_PUT, "/me/player", body, &resp, 0, "control", result);
            free(body);
            cJSON_Delete(root);
            sr_http_resp_clear(&resp);
            if (ok) set_result(result, 1, "已切换播放设备");
            return ok;
        }
        case SR_CMD_PLAY_URI: {
            cJSON *root = cJSON_CreateObject();
            cJSON *uris = cJSON_AddArrayToObject(root, "uris");
            cJSON_AddItemToArray(uris, cJSON_CreateString(command->uri));
            char *body = cJSON_PrintUnformatted(root);
            sr_http_resp_t resp = {0};
            const bool ok = body && sp_call(sp, SR_M_PUT, "/me/player/play", body, &resp, 0,
                                            "control", result);
            free(body);
            cJSON_Delete(root);
            sr_http_resp_clear(&resp);
            if (ok) set_result(result, 1, "开始播放");
            return ok;
        }
        case SR_CMD_PLAY_CONTEXT: {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "context_uri", command->uri);
            if (command->value >= 0) {
                cJSON *offset = cJSON_AddObjectToObject(root, "offset");
                cJSON_AddNumberToObject(offset, "position", command->value);
            }
            char *body = cJSON_PrintUnformatted(root);
            sr_http_resp_t resp = {0};
            const bool ok = body && sp_call(sp, SR_M_PUT, "/me/player/play", body, &resp, 0,
                                            "control", result);
            free(body);
            cJSON_Delete(root);
            sr_http_resp_clear(&resp);
            if (ok) set_result(result, 1, "开始播放");
            return ok;
        }
        default:
            set_result(result, 1, "不支持的操作");
            return false;
    }
}
