// main/sr_spotify.h
// Spotify Web API 封装：令牌刷新、429 记录、列表分页、控制指令。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sr_http.h"
#include "sr_playback.h"
#include "sr_store.h"

// 列表容量：既要装下 6 行分页列表（+「播放全部」占位），也要装下 13 项菜单。
// 菜单在 sr_app.c 里复用同一个 sr_list_t，所以这里必须是 13 而不是 12。
#define SR_LIST_MAX 13
#define SR_LIST_TITLE_CAP 90
#define SR_LIST_SUB_CAP 60

typedef struct {
    char title[SR_LIST_TITLE_CAP];
    char subtitle[SR_LIST_SUB_CAP];
    char uri[SR_URI_CAP];
    char context_uri[SR_URI_CAP];
    int position;      // 在 context 内的绝对位置；-1 表示无上下文
    bool actionable;
} sr_item_t;

typedef struct {
    sr_item_t items[SR_LIST_MAX];
    int count;
    int total;
    int offset;
} sr_list_t;

typedef struct {
    int status;        // 0=网络错误，1=成功
    char text[96];
} sr_result_t;

typedef struct {
    sr_config_t *cfg;
    int64_t blocked_until_us;
    bool blocked_quota;   // 封禁原因：true=开发模式配额用尽，false=临时限流
    bool auth_failed;
} sr_spotify_t;

typedef enum {
    SR_CMD_PLAY_PAUSE = 0,
    SR_CMD_NEXT,
    SR_CMD_PREV,
    SR_CMD_VOLUME,
    SR_CMD_SEEK,
    SR_CMD_SHUFFLE,
    SR_CMD_REPEAT,
    SR_CMD_TRANSFER,
    SR_CMD_PLAY_URI,
    SR_CMD_PLAY_CONTEXT,
} sr_cmd_t;

typedef struct {
    sr_cmd_t type;
    int value;             // 音量/进度/循环模式
    char uri[SR_URI_CAP];
    char device_id[64];
} sr_command_t;

void sr_sp_init(sr_spotify_t *sp, sr_config_t *cfg);
bool sr_sp_blocked(const sr_spotify_t *sp, int64_t now_us);
int64_t sr_sp_block_left_ms(const sr_spotify_t *sp, int64_t now_us);
void sr_sp_clear_block(sr_spotify_t *sp);

// 读取与命令；返回 false 时 result 已填好用户可读原因。
bool sr_sp_load_playback(sr_spotify_t *sp, sr_playback_t *out, sr_result_t *result);
bool sr_sp_load_devices(sr_spotify_t *sp, sr_list_t *out, sr_result_t *result);
bool sr_sp_load_playlists(sr_spotify_t *sp, int offset, int limit, sr_list_t *out,
                          sr_result_t *result);
bool sr_sp_load_playlist_items(sr_spotify_t *sp, const char *playlist_id, const char *playlist_uri,
                               int offset, int limit, sr_list_t *out, sr_result_t *result);
bool sr_sp_load_queue(sr_spotify_t *sp, sr_list_t *out, sr_result_t *result);
bool sr_sp_load_recent(sr_spotify_t *sp, sr_list_t *out, sr_result_t *result);
bool sr_sp_liked(sr_spotify_t *sp, const char *uri, bool *liked, sr_result_t *result);
bool sr_sp_set_like(sr_spotify_t *sp, const char *uri, bool liked, sr_result_t *result);
bool sr_sp_command(sr_spotify_t *sp, const sr_command_t *command, sr_result_t *result);
bool sr_sp_refresh_tokens(sr_spotify_t *sp, sr_result_t *result);
bool sr_sp_token_expiring(const sr_spotify_t *sp, int64_t now_epoch_ms);
