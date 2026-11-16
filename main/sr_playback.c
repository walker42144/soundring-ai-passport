#include "sr_playback.h"

#include "cJSON.h"
#include "sr_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    const size_t length = strlen(src);
    if (length < cap) {
        memcpy(dst, src, length + 1);
    } else {
        sr_utf8_ellipsis(src, dst, cap);
    }
}

void sr_playback_reset(sr_playback_t *playback) {
    if (!playback) return;
    memset(playback, 0, sizeof(*playback));
    playback->volume = -1;
    copy_str(playback->device_name, sizeof(playback->device_name), "无播放设备");
    copy_str(playback->title, sizeof(playback->title), "没有活跃播放");
    copy_str(playback->artist, sizeof(playback->artist), "请在手机或电脑打开 Spotify");
    copy_str(playback->repeat, sizeof(playback->repeat), "off");
}

static const char *json_string(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static int json_int(const cJSON *object, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool json_true(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsTrue(item);
}

// Prefer a compact cover close to the 120px display area to limit TLS heap use.
static const char *pick_cover(const cJSON *images) {
    if (!cJSON_IsArray(images)) return NULL;
    const cJSON *best = NULL;
    int best_score = 1 << 30;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, images) {
        const char *url = json_string(entry, "url");
        if (!url || strncmp(url, "https://", 8) != 0) continue;
        const int width = json_int(entry, "width", 300);
        int score = abs(width - 120);
        if (width < 48) score += 400;  // Avoid very small thumbnails unless no better choice exists.
        if (score < best_score) {
            best_score = score;
            best = entry;
        }
    }
    return best ? json_string(best, "url") : NULL;
}

static uint32_t parse_disallows(const cJSON *actions) {
    uint32_t mask = 0;
    const cJSON *flags = cJSON_GetObjectItemCaseSensitive(actions, "disallows");
    if (!cJSON_IsObject(flags)) return 0;
    if (json_true(flags, "pausing")) mask |= SR_ACT_PAUSING;
    if (json_true(flags, "resuming")) mask |= SR_ACT_RESUMING;
    if (json_true(flags, "seeking") || json_true(flags, "peeking_next") ||
        json_true(flags, "peeking_prev")) {
        mask |= SR_ACT_SEEKING;
    }
    if (json_true(flags, "skipping_prev")) mask |= SR_ACT_PREV;
    if (json_true(flags, "skipping_next")) mask |= SR_ACT_NEXT;
    if (json_true(flags, "toggling_shuffle")) mask |= SR_ACT_SHUFFLE;
    if (json_true(flags, "toggling_repeat_context") || json_true(flags, "toggling_repeat_track")) {
        mask |= SR_ACT_REPEAT;
    }
    if (json_true(flags, "transferring_playback")) mask |= SR_ACT_TRANSFER;
    return mask;
}

bool sr_playback_parse(const char *json, sr_playback_t *out) {
    if (!out) return false;
    sr_playback_reset(out);
    if (!json || !*json) return true;  // 204/空体：确实没有活跃播放

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
    if (cJSON_IsObject(device)) {
        const char *id = json_string(device, "id");
        const char *name = json_string(device, "name");
        if (id && *id) {
            copy_str(out->device_id, sizeof(out->device_id), id);
            out->has_device = true;
        }
        if (name && *name) copy_str(out->device_name, sizeof(out->device_name), name);
        out->supports_volume = json_true(device, "supports_volume");
        const cJSON *volume = cJSON_GetObjectItemCaseSensitive(device, "volume_percent");
        out->volume = cJSON_IsNumber(volume) ? volume->valueint : -1;
        out->restricted = json_true(device, "is_restricted");
    }

    out->playing = json_true(root, "is_playing");
    out->shuffle = json_true(root, "shuffle_state");
    const char *repeat = json_string(root, "repeat_state");
    if (repeat) copy_str(out->repeat, sizeof(out->repeat), repeat);
    out->position_ms = json_int(root, "progress_ms", 0);

    if (cJSON_IsObject(item)) {
        const char *name = json_string(item, "name");
        if (name && *name) copy_str(out->title, sizeof(out->title), name);
        const char *uri = json_string(item, "uri");
        if (uri && *uri) copy_str(out->uri, sizeof(out->uri), uri);
        out->duration_ms = json_int(item, "duration_ms", 0);

        const cJSON *artists = cJSON_GetObjectItemCaseSensitive(item, "artists");
        char joined[SR_ARTIST_CAP * 2] = "";
        size_t used = 0;
        const cJSON *artist = NULL;
        cJSON_ArrayForEach(artist, artists) {
            const char *artist_name = json_string(artist, "name");
            if (!artist_name || !*artist_name) continue;
            if (used && used + 3 < sizeof(joined)) {
                memcpy(joined + used, " / ", 3);
                used += 3;
            }
            const size_t length = strlen(artist_name);
            if (used + length + 1 >= sizeof(joined)) break;
            memcpy(joined + used, artist_name, length);
            used += length;
            joined[used] = '\0';
        }
        if (joined[0]) {
            copy_str(out->artist, sizeof(out->artist), joined);
        } else {
            const cJSON *show = cJSON_GetObjectItemCaseSensitive(item, "show");
            const char *show_name = show ? json_string(show, "name") : NULL;
            copy_str(out->artist, sizeof(out->artist), show_name && *show_name ? show_name : "Spotify");
        }

        const cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "album");
        const char *album_name = album ? json_string(album, "name") : NULL;
        if (album_name && *album_name) copy_str(out->album, sizeof(out->album), album_name);

        const cJSON *images = album ? cJSON_GetObjectItemCaseSensitive(album, "images") : NULL;
        if (!cJSON_IsArray(images)) images = cJSON_GetObjectItemCaseSensitive(item, "images");
        const char *cover = pick_cover(images);
        if (cover) copy_str(out->cover_url, sizeof(out->cover_url), cover);
    }

    out->disallowed = parse_disallows(cJSON_GetObjectItemCaseSensitive(root, "actions"));
    cJSON_Delete(root);
    return true;
}

bool sr_playback_allows(const sr_playback_t *playback, sr_action_bit_t action) {
    if (!playback || !playback->has_device || playback->restricted) return false;
    return (playback->disallowed & (uint32_t)action) == 0;
}
