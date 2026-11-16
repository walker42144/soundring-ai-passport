// main/sr_playback.h
// /me/player 的领域模型与解析（字段来自手表版 Playback.parse，按 2026 API 迁移补齐）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SR_DEVICE_NAME_CAP 48
#define SR_TITLE_CAP 96
#define SR_ARTIST_CAP 96
#define SR_ALBUM_CAP 64
#define SR_URI_CAP 80
#define SR_COVER_URL_CAP 200

typedef enum {
    SR_ACT_PAUSING = 1u << 0,
    SR_ACT_RESUMING = 1u << 1,
    SR_ACT_SEEKING = 1u << 2,
    SR_ACT_PREV = 1u << 3,
    SR_ACT_NEXT = 1u << 4,
    SR_ACT_SHUFFLE = 1u << 5,
    SR_ACT_REPEAT = 1u << 6,
    SR_ACT_TRANSFER = 1u << 7,
    SR_ACT_VOLUME = 1u << 8,
} sr_action_bit_t;

typedef struct {
    char device_id[64];
    char device_name[SR_DEVICE_NAME_CAP];
    bool playing;
    char title[SR_TITLE_CAP];
    char artist[SR_ARTIST_CAP];
    char album[SR_ALBUM_CAP];
    char uri[SR_URI_CAP];
    char cover_url[SR_COVER_URL_CAP];
    int32_t duration_ms;
    int32_t position_ms;
    int volume;           // -1 表示设备不报音量
    bool supports_volume;
    bool restricted;
    bool shuffle;
    char repeat[8];       // off | context | track
    uint32_t disallowed;  // sr_action_bit_t 位图
    bool has_device;
} sr_playback_t;

void sr_playback_reset(sr_playback_t *playback);
// 解析失败返回 false；缺字段按“无活跃播放”处理，绝不留下半更新的旧数据。
bool sr_playback_parse(const char *json, sr_playback_t *out);
bool sr_playback_allows(const sr_playback_t *playback, sr_action_bit_t action);
