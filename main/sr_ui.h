// main/sr_ui.h
// 240x320 竖屏渲染层：只负责绘制当前视图，不持有业务状态。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sr_spotify.h"
#include "sr_progress.h"

typedef enum {
    SR_SC_HOME = 0,
    SR_SC_MENU,
    SR_SC_LIBRARY,
    SR_SC_CONTROLS,
    SR_SC_PLAYLISTS,
    SR_SC_TRACKS,
    SR_SC_DEVICES,
    SR_SC_QUEUE,
    SR_SC_RECENT,
    SR_SC_VOLUME,
    SR_SC_SEEK,
    SR_SC_SETTINGS,
    SR_SC_HELP,
    SR_SC_PAIRING,
    SR_SC_BRIGHTNESS,
    SR_SC_IMMERSIVE,
    SR_SC_STATUS,
} sr_screen_t;

#define SR_VIEW_ROWS 6

typedef struct {
    sr_screen_t screen;
    const char *title;
    const sr_list_t *list;
    int list_top;
    int sel;
    int offset;
    int total;

    const sr_playback_t *playback;
    int32_t position_ms;
    const sr_progress_t *progress;
    bool liked;
    bool like_known;
    bool playback_stale;
    const char (*status_lines)[64];
    int status_page;

    int draft;           // 音量/进度草稿
    bool dirty;          // 草稿未提交

    bool connected;
    bool time_valid;
    bool auth_ok;
    bool auth_failed;
    int battery;

    bool pairing_active;
    int pair_seconds;
    const char *pair_status;
    const char *pair_code;

    int page;
    int pages;

    uint8_t brightness;
    uint16_t dim_sec;
    const char *toast;
} sr_view_t;

void sr_ui_init(void);
// rebuild=true 时重建容器；否则只更新文本/进度。
void sr_ui_render(const sr_view_t *view, bool rebuild);
void sr_ui_set_toast_color(bool error);
// 封面位图变化后由应用调用（持 LVGL 锁）。
void sr_ui_attach_cover(void);
void sr_ui_pause_progress(void);
