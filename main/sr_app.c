#include "sr_app.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sr_cover.h"
#include "sr_net.h"
#include "sr_input.h"
#include "sr_navigation.h"
#include "sr_pair.h"
#include "sr_playback.h"
#include "sr_policy.h"
#include "sr_progress.h"
#include "sr_spotify.h"
#include "sr_store.h"
#include "sr_text.h"
#include "sr_ui.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sr_app";
#define SR_PAGE 6
#define SR_SETTINGS_COUNT 8
#define SR_NAV_DEPTH 4
#define SR_JOB_DEPTH 6
// 首页进度 tick：只重绘文本与进度条，不发请求。播放中 1s，暂停 5s。
#define SR_TICK_PLAY_US 1000000
#define SR_TICK_PAUSE_US 5000000

typedef struct {
    sr_screen_t screen;
    int sel;
    int offset;
    int list_top;
    char title[40];
    char playlist_id[48];
    char playlist_uri[80];
} nav_entry_t;

typedef enum {
    JOB_POLL = 0,
    JOB_COMMAND,
    JOB_LIST,
    JOB_COVER,
    JOB_LIKE_CHECK,
    JOB_LIKE_SET,
} job_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t arg;
    int value;
    // 装封面 URL（最长 SR_COVER_URL_CAP）；命令/列表 job 里的 URI 只用到前 80 字节。
    char text[SR_COVER_URL_CAP];
    char extra[64];
} sr_job_t;

static sr_config_t s_cfg;
static sr_spotify_t s_sp;
static sr_playback_t s_pb;
static sr_progress_t s_progress;
static sr_list_t s_list;
static sr_screen_t s_screen = SR_SC_HOME;
static int s_sel;
static int s_offset;
static int s_list_top;
static int s_total;
static char s_title[40];
static char s_playlist_id[48];
static char s_playlist_uri[80];
static char s_toast[72];
static int64_t s_toast_until_us;
static bool s_toast_error;
static bool s_liked;
static bool s_like_known;
static bool s_like_pending;
static bool s_like_after_check;
static int s_volume_draft = -1;
static int s_seek_draft;
static int s_brightness_draft = 80;
static int s_brightness_original = 80;
static bool s_dirty;
static int s_settings_confirm;
static char s_pair_status[48] = "等待电脑";

static nav_entry_t s_nav[SR_NAV_DEPTH];
static int s_nav_depth;

static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_mutex;
static sr_gate_t s_gate;
static int64_t s_next_poll_us;
static int64_t s_next_tick_us;   // 首页进度 tick；与网络轮询完全独立
static int s_poll_failures;
static int64_t s_last_key_us;
static bool s_display_suspended;
static bool s_dimmed;
static uint8_t s_saved_brightness = 80;
static int s_battery = -1;
static int64_t s_next_battery_us;
static bool s_render_pending;
static bool s_force_rebuild = true;
static bool s_cover_pending;
static int64_t s_cover_retry_us;
static bool s_auth_provisioned;
static char s_cover_url[SR_COVER_URL_CAP];
static int64_t s_boot_us;
static bool s_diag_logged;

// 循环模式：0=关 1=列表(context) 2=单曲(track)。
static const char *const k_repeat_names[3] = {"关", "列表", "单曲"};

static void toast_locked(const char *text, bool error) {
    sr_utf8_ellipsis(text ? text : "", s_toast, sizeof(s_toast));
    s_toast_until_us = esp_timer_get_time() + 3000000;
    s_toast_error = error;
}

static bool enqueue(uint8_t kind, uint8_t arg, int value, const char *text, const char *extra) {
    if (!s_jobs) return false;
    sr_job_t job = {.kind = kind, .arg = arg, .value = value};
    if (text) sr_utf8_ellipsis(text, job.text, sizeof(job.text));
    if (extra) sr_utf8_ellipsis(extra, job.extra, sizeof(job.extra));
    const BaseType_t queued = kind == JOB_LIST ? xQueueSendToFront(s_jobs, &job, 0)
                                              : xQueueSend(s_jobs, &job, 0);
    if (queued != pdTRUE) {
        toast_locked("请求过多，稍后再试", true);
        return false;
    }
    return true;
}

const char *sr_app_pair_code(void) { return sr_pair_code(); }

static void log_heap(const char *stage) {
    ESP_LOGI(TAG, "%s: free=%u largest=%u min=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

static int page_count(void) {
    if (s_total <= 0) return 1;
    return (s_total + SR_PAGE - 1) / SR_PAGE;
}

static void build_view_locked(sr_view_t *view, bool *rebuild) {
    memset(view, 0, sizeof(*view));
    view->screen = s_screen;
    view->title = s_title;
    view->list = &s_list;
    view->list_top = s_list_top;
    view->sel = s_sel;
    view->offset = s_offset;
    view->total = s_total;
    view->page = s_offset / SR_PAGE + 1;
    view->pages = page_count();
    view->playback = &s_pb;
    view->position_ms = (int32_t)sr_progress_position(&s_progress, esp_timer_get_time());
    view->progress = &s_progress;
    view->liked = s_liked;
    view->like_known = s_like_known;
    view->draft = s_screen == SR_SC_SEEK ? s_seek_draft
                  : s_screen == SR_SC_BRIGHTNESS ? s_brightness_draft
                                                 : s_volume_draft;
    view->dirty = s_dirty;
    view->connected = sr_net_connected();
    view->time_valid = sr_net_time_valid();
    view->auth_ok = sr_store_has_auth(&s_cfg);
    view->auth_failed = s_sp.auth_failed;
    view->battery = s_battery;
    view->pairing_active = sr_pair_active();
    view->pair_seconds = sr_pair_seconds_left();
    view->pair_status = s_pair_status;
    view->pair_code = sr_pair_code();
    view->brightness = s_cfg.brightness;
    view->dim_sec = s_cfg.dim_sec;
    view->toast = (esp_timer_get_time() < s_toast_until_us) ? s_toast : NULL;
    *rebuild = s_force_rebuild;
    s_force_rebuild = false;
}

static void render_locked(void) {
    if (s_display_suspended) return;
    sr_view_t view;
    bool rebuild = false;
    build_view_locked(&view, &rebuild);
    if (!bsp_lvgl_lock(400)) {
        s_render_pending = true;
        s_force_rebuild = rebuild;
        return;
    }
    sr_ui_render(&view, rebuild);
    sr_ui_set_toast_color(s_toast_error);
    bsp_lvgl_unlock();
    s_render_pending = false;
}

static void mark_rebuild(void) { s_force_rebuild = true; }

// 当前循环模式索引：无法识别的值按“关”处理。
static int repeat_mode_locked(void) {
    if (strcmp(s_pb.repeat, "context") == 0) return 1;
    if (strcmp(s_pb.repeat, "track") == 0) return 2;
    return 0;
}

static bool is_navigation_screen(sr_screen_t screen) {
    return screen == SR_SC_MENU || screen == SR_SC_LIBRARY || screen == SR_SC_CONTROLS;
}
static void fill_menu_list(void) {
    memset(&s_list, 0, sizeof(s_list));
    sr_nav_group_t group = s_screen == SR_SC_LIBRARY ? SR_NAV_LIBRARY :
                           s_screen == SR_SC_CONTROLS ? SR_NAV_CONTROLS : SR_NAV_ROOT;
    const size_t count = sr_nav_count(group);
    for (size_t i = 0; i < count && i < SR_LIST_MAX; ++i) {
        const sr_nav_item_t *entry = sr_nav_item(group, i);
        sr_item_t *item = &s_list.items[i];
        sr_utf8_ellipsis(entry->title, item->title, sizeof(item->title));
        const char *subtitle = entry->subtitle;
        if (entry->action == SR_NAV_SHUFFLE) subtitle = s_pb.shuffle ? "已开启" : "已关闭";
        if (entry->action == SR_NAV_REPEAT) subtitle = k_repeat_names[repeat_mode_locked()];
        sr_utf8_ellipsis(subtitle, item->subtitle, sizeof(item->subtitle));
        item->actionable = true;
        item->position = (int)entry->action;
    }
    s_list.count = (int)count; s_list.total = (int)count; s_total = 0; s_offset = 0;
}

static void fill_settings_list(void) {
    memset(&s_list, 0, sizeof(s_list));
    char value[24];

    snprintf(value, sizeof(value), "%d%%", s_cfg.brightness);
    sr_utf8_ellipsis("亮度", s_list.items[0].title, sizeof(s_list.items[0].title));
    sr_utf8_ellipsis(value, s_list.items[0].subtitle, sizeof(s_list.items[0].subtitle));

    if (s_cfg.dim_sec == 0) snprintf(value, sizeof(value), "不自动");
    else snprintf(value, sizeof(value), "%u 秒", (unsigned)s_cfg.dim_sec);
    sr_utf8_ellipsis("自动息屏", s_list.items[1].title, sizeof(s_list.items[1].title));
    sr_utf8_ellipsis(value, s_list.items[1].subtitle, sizeof(s_list.items[1].subtitle));

    sr_utf8_ellipsis("立即息屏", s_list.items[2].title, sizeof(s_list.items[2].title));
    sr_utf8_ellipsis("按确定", s_list.items[2].subtitle, sizeof(s_list.items[2].subtitle));

    sr_utf8_ellipsis("同步速度", s_list.items[3].title, sizeof(s_list.items[3].title));
    sr_utf8_ellipsis(s_cfg.poll_mode == SR_POLL_QUICK ? "快速" :
                     s_cfg.poll_mode == SR_POLL_NORMAL ? "标准" : "省电", s_list.items[3].subtitle,
                     sizeof(s_list.items[3].subtitle));

    sr_utf8_ellipsis("电脑配对", s_list.items[4].title, sizeof(s_list.items[4].title));
    sr_utf8_ellipsis("USB", s_list.items[4].subtitle, sizeof(s_list.items[4].subtitle));

    sr_utf8_ellipsis("清除授权", s_list.items[5].title, sizeof(s_list.items[5].title));
    sr_utf8_ellipsis(s_settings_confirm ? "再按确定" : "需确认", s_list.items[5].subtitle,
                     sizeof(s_list.items[5].subtitle));

    sr_utf8_ellipsis("使用说明", s_list.items[6].title, sizeof(s_list.items[6].title));
    sr_utf8_ellipsis("按键", s_list.items[6].subtitle, sizeof(s_list.items[6].subtitle));

    sr_utf8_ellipsis("返回", s_list.items[7].title, sizeof(s_list.items[7].title));
    for (int i = 0; i < SR_SETTINGS_COUNT; ++i) {
        s_list.items[i].actionable = true;
        s_list.items[i].position = i;
    }
    s_list.count = SR_SETTINGS_COUNT;
    s_list.total = SR_SETTINGS_COUNT;
    s_total = 0;
    s_offset = 0;
}

static void push_current_locked(void) {
    if (s_nav_depth >= SR_NAV_DEPTH) return;
    nav_entry_t *entry = &s_nav[s_nav_depth++];
    entry->screen = s_screen;
    entry->sel = s_sel;
    entry->offset = s_offset;
    entry->list_top = s_list_top;
    sr_utf8_ellipsis(s_title, entry->title, sizeof(entry->title));
    sr_utf8_ellipsis(s_playlist_id, entry->playlist_id, sizeof(entry->playlist_id));
    sr_utf8_ellipsis(s_playlist_uri, entry->playlist_uri, sizeof(entry->playlist_uri));
}

static void enter_screen_locked(sr_screen_t screen, const char *title, int offset) {
    push_current_locked();
    s_screen = screen;
    s_sel = 0;
    s_offset = offset;
    s_list_top = 0;
    s_dirty = false;
    s_settings_confirm = 0;
    sr_utf8_ellipsis(title ? title : "", s_title, sizeof(s_title));
    memset(&s_list, 0, sizeof(s_list));
    s_total = 0;
    mark_rebuild();

    if (is_navigation_screen(screen)) {
        fill_menu_list();
        return;
    }
    if (screen == SR_SC_SETTINGS) {
        fill_settings_list();
        return;
    }
    if (screen == SR_SC_PAIRING) {
        sr_pair_begin();
        sr_utf8_ellipsis("等待电脑", s_pair_status, sizeof(s_pair_status));
        return;
    }
    if (screen == SR_SC_QUEUE || screen == SR_SC_RECENT || screen == SR_SC_PLAYLISTS ||
        screen == SR_SC_DEVICES || screen == SR_SC_TRACKS) {
        enqueue(JOB_LIST, (uint8_t)screen, offset, NULL, NULL);
    }
    if (screen == SR_SC_VOLUME) {
        s_volume_draft = s_pb.volume >= 0 ? s_pb.volume : 50;
    }
    if (screen == SR_SC_SEEK) {
        s_seek_draft = (int32_t)sr_progress_position(&s_progress, esp_timer_get_time());
    }
    if (screen == SR_SC_BRIGHTNESS) {
        s_brightness_original = s_cfg.brightness;
        s_brightness_draft = s_cfg.brightness;
    }
}

static void go_back_locked(void) {
    if (s_screen == SR_SC_PAIRING) sr_pair_end();
    if (s_nav_depth == 0) {
        s_screen = SR_SC_HOME;
        s_sel = 0;
        s_offset = 0;
        s_list_top = 0;
        s_dirty = false;
        mark_rebuild();
        s_next_poll_us = esp_timer_get_time();
        return;
    }
    const nav_entry_t entry = s_nav[--s_nav_depth];
    s_screen = entry.screen;
    s_sel = entry.sel;
    s_offset = entry.offset;
    s_list_top = entry.list_top;
    sr_utf8_ellipsis(entry.title, s_title, sizeof(s_title));
    sr_utf8_ellipsis(entry.playlist_id, s_playlist_id, sizeof(s_playlist_id));
    sr_utf8_ellipsis(entry.playlist_uri, s_playlist_uri, sizeof(s_playlist_uri));
    s_dirty = false;
    mark_rebuild();

    if (is_navigation_screen(s_screen)) {
        fill_menu_list();
    } else if (s_screen == SR_SC_SETTINGS) {
        fill_settings_list();
    } else if (s_screen == SR_SC_PLAYLISTS || s_screen == SR_SC_TRACKS ||
               s_screen == SR_SC_DEVICES || s_screen == SR_SC_QUEUE || s_screen == SR_SC_RECENT) {
        enqueue(JOB_LIST, (uint8_t)s_screen, s_offset, NULL, NULL);
    } else if (s_screen == SR_SC_HOME) {
        s_next_poll_us = esp_timer_get_time();
    }
}

// 返回 true 表示指令已排入队列（调用方可以立刻提示“已发送”）。
static bool send_command_locked(sr_cmd_t type, int value, const char *uri, const char *device) {
    const int64_t now = esp_timer_get_time();
    if (!sr_gate_acquire(&s_gate, now)) {
        toast_locked("操作太快，请稍候", true);
        render_locked();
        return false;
    }
    if (!enqueue(JOB_COMMAND, (uint8_t)type, value, uri, device)) {
        // 队列满：指令根本没发出去，立刻放开 800ms 门控，否则后续指令会被永久拒绝。
        sr_gate_release(&s_gate, esp_timer_get_time());
        return false;
    }
    return true;
}

static void adjust_volume_locked(int delta) {
    if (!s_pb.supports_volume || s_pb.volume < 0) {
        toast_locked("当前设备不支持音量", true);
        render_locked();
        return;
    }
    const int base = s_volume_draft >= 0 ? s_volume_draft : s_pb.volume;
    int target = base + delta;
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    s_volume_draft = target;
    send_command_locked(SR_CMD_VOLUME, target, NULL, s_pb.device_id);
    s_next_poll_us = esp_timer_get_time() + 800000;
}

static void toggle_like_locked(void) {
    if (s_pb.uri[0] == '\0' || strncmp(s_pb.uri, "spotify:track:", 14) != 0) {
        toast_locked("当前内容不支持收藏", true);
        return;
    }
    if (!s_like_known) {
        if (s_like_after_check) {
            toast_locked("正在同步收藏状态", false);
            return;
        }
        s_like_after_check = enqueue(JOB_LIKE_CHECK, 0, 0, s_pb.uri, NULL);
        if (s_like_after_check) toast_locked("正在同步收藏状态", false);
        return;
    }
    if (s_like_pending) {
        toast_locked("正在更新收藏，请稍候", false);
        return;
    }
    if (enqueue(JOB_LIKE_SET, 0, s_liked ? 0 : 1, s_pb.uri, NULL)) {
        s_like_pending = true;
        toast_locked("正在更新收藏", false);
    }
}

// 面板开关只在持 LVGL 锁时操作，避免驱动层与渲染并发。
// 顺序固定：先关显示（关闭扫描，GRAM 保留）再关背光，避免关背光瞬间漏出一帧白场。
// 返回 true 表示“面板已处于目标状态”（含没有接管到面板的情况），false 表示切换失败。
static bool panel_display_on(bool on) {
    const esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) {
        // 没接管到面板（例如显示未初始化）：背光路径仍然有效，不能因此判定唤醒失败。
        ESP_LOGW(TAG, "面板句柄不可用，跳过显示%s", on ? "打开" : "关闭");
        return true;
    }
    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "LVGL 锁超时，面板显示未%s", on ? "打开" : "关闭");
        return false;
    }
    const esp_err_t err = esp_lcd_panel_disp_on_off(panel, on);
    bsp_lvgl_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "面板显示%s失败：%s", on ? "打开" : "关闭", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void sleep_now_locked(void) {
    s_dimmed = false;
    if (bsp_lvgl_lock(300)) {
        sr_ui_pause_progress();
        bsp_lvgl_unlock();
    }
    // 关显示失败不影响息屏：背光照样关掉，屏幕至少是黑的，下次仍可走唤醒路径。
    (void)panel_display_on(false);
    bsp_display_backlight(0);
    s_display_suspended = true;
    ESP_LOGI(TAG, "息屏（关闭显示与背光，重绘与同步暂停）");
}

static void settings_activate_locked(void) {
    switch (s_sel) {
        case 0: {
            char title[40];
            sr_utf8_ellipsis("亮度", title, sizeof(title));
            enter_screen_locked(SR_SC_BRIGHTNESS, title, 0);
            break;
        }
        case 1:
            if (s_cfg.dim_sec == 0) s_cfg.dim_sec = 30;
            else if (s_cfg.dim_sec == 30) s_cfg.dim_sec = 60;
            else if (s_cfg.dim_sec == 60) s_cfg.dim_sec = 120;
            else s_cfg.dim_sec = 0;
            (void)sr_store_save_settings(&s_cfg);
            break;
        case 2:
            sleep_now_locked();
            break;
        case 3:
            s_cfg.poll_mode = (s_cfg.poll_mode + 1) % 3;
            (void)sr_store_save_settings(&s_cfg);
            break;
        case 4: {
            char title[40];
            sr_utf8_ellipsis("电脑配对", title, sizeof(title));
            enter_screen_locked(SR_SC_PAIRING, title, 0);
            break;
        }
        case 5:
            if (s_settings_confirm) {
                (void)sr_store_clear_auth();
                memset(s_cfg.access_token, 0, sizeof(s_cfg.access_token));
                memset(s_cfg.refresh_token, 0, sizeof(s_cfg.refresh_token));
                s_cfg.expires_at_ms = 0;
                s_settings_confirm = 0;
                toast_locked("已清除授权", false);
                char title[40];
                sr_utf8_ellipsis("电脑配对", title, sizeof(title));
                enter_screen_locked(SR_SC_PAIRING, title, 0);
            } else {
                s_settings_confirm = 1;
                toast_locked("再按一次确定清除授权", true);
            }
            break;
        case 6:
            enter_screen_locked(SR_SC_HELP, "使用说明", 0);
            break;
        default:
            go_back_locked();
            break;
    }
    // 只有仍停留在设置页时才重填列表，避免覆盖刚进入的子页面状态。
    if (s_screen == SR_SC_SETTINGS) {
        fill_settings_list();
        mark_rebuild();
    }
}

static void menu_activate_locked(void) {
    if (s_sel < 0 || s_sel >= s_list.count) return;
    switch ((sr_nav_action_t)s_list.items[s_sel].position) {
        case SR_NAV_MUSIC: enter_screen_locked(SR_SC_LIBRARY, "音乐库", 0); break;
        case SR_NAV_DEVICES: enter_screen_locked(SR_SC_DEVICES, "播放设备", 0); break;
        case SR_NAV_CONTROL: enter_screen_locked(SR_SC_CONTROLS, "播放控制", 0); break;
        case SR_NAV_SETTINGS: enter_screen_locked(SR_SC_SETTINGS, "设置", 0); break;
        case SR_NAV_SLEEP: sleep_now_locked(); break;
        case SR_NAV_PLAYLISTS: enter_screen_locked(SR_SC_PLAYLISTS, "歌单", 0); break;
        case SR_NAV_RECENT: enter_screen_locked(SR_SC_RECENT, "最近播放", 0); break;
        case SR_NAV_QUEUE: enter_screen_locked(SR_SC_QUEUE, "播放队列", 0); break;
        case SR_NAV_VOLUME: enter_screen_locked(SR_SC_VOLUME, "音量", 0); break;
        case SR_NAV_SEEK: enter_screen_locked(SR_SC_SEEK, "播放进度", 0); break;
        case SR_NAV_LIKE: toggle_like_locked(); break;
        case SR_NAV_SHUFFLE:
            if (send_command_locked(SR_CMD_SHUFFLE, !s_pb.shuffle, NULL, s_pb.device_id)) {
                toast_locked("正在更新随机播放", false); s_next_poll_us = esp_timer_get_time() + 250000;
            }
            break;
        case SR_NAV_REPEAT:
            if (send_command_locked(SR_CMD_REPEAT, (repeat_mode_locked()+1)%3, NULL, s_pb.device_id)) {
                toast_locked("正在更新循环模式", false); s_next_poll_us = esp_timer_get_time() + 250000;
            }
            break;
        default: break;
    }
}

static void list_activate_locked(void) {
    if (s_sel < 0 || s_sel >= s_list.count) return;
    const sr_item_t *item = &s_list.items[s_sel];
    if (s_screen == SR_SC_PLAYLISTS) {
        if (!item->actionable) {
            toast_locked("该歌单不可读", true);
            return;
        }
        sr_utf8_ellipsis(item->context_uri, s_playlist_id, sizeof(s_playlist_id));
        sr_utf8_ellipsis(item->uri, s_playlist_uri, sizeof(s_playlist_uri));
        enter_screen_locked(SR_SC_TRACKS, item->title, 0);
    } else if (s_screen == SR_SC_TRACKS) {
        if (!item->actionable) {
            toast_locked("此曲目不可播放", true);
            return;
        }
        if (item->position < 0) {
            // “播放全部”：context_uri + 不带 offset，由 Spotify 从第一首开始。
            send_command_locked(SR_CMD_PLAY_CONTEXT, -1, item->uri, s_pb.device_id);
        } else {
            send_command_locked(SR_CMD_PLAY_CONTEXT, item->position, s_playlist_uri, s_pb.device_id);
        }
        toast_locked("正在开始播放", false);
    } else if (s_screen == SR_SC_DEVICES) {
        if (!item->actionable) {
            toast_locked("已是当前设备", false);
            return;
        }
        send_command_locked(SR_CMD_TRANSFER, 0, NULL, item->uri);
        toast_locked("正在切换设备", false);
    } else if (s_screen == SR_SC_RECENT) {
        if (!item->actionable) {
            toast_locked("此内容不可播放", true);
            return;
        }
        send_command_locked(SR_CMD_PLAY_URI, 0, item->uri, s_pb.device_id);
        toast_locked("正在开始播放", false);
    } else if (s_screen == SR_SC_QUEUE) {
        enqueue(JOB_LIST, (uint8_t)SR_SC_QUEUE, 0, NULL, NULL);
        toast_locked("已刷新队列", false);
    } else if (is_navigation_screen(s_screen)) {
        // All navigation actions are defined by sr_navigation.
        menu_activate_locked();
    } else if (s_screen == SR_SC_SETTINGS) {
        settings_activate_locked();
    }
}

static void move_selection_locked(int delta) {
    if (is_navigation_screen(s_screen) || s_screen == SR_SC_SETTINGS) {
        const int count = s_list.count;
        if (count == 0) return;
        s_sel = (s_sel + (delta % count) + count) % count;
    } else {
        if (s_list.count == 0) return;
        s_sel += delta;
        if (s_sel < 0) s_sel = 0;
        if (s_sel >= s_list.count) s_sel = s_list.count - 1;
    }
    if (s_sel < s_list_top) s_list_top = s_sel;
    if (s_sel >= s_list_top + SR_VIEW_ROWS) s_list_top = s_sel - SR_VIEW_ROWS + 1;
    if (s_list_top < 0) s_list_top = 0;
}

static void page_move_locked(int direction) {
    if (is_navigation_screen(s_screen) || s_screen == SR_SC_SETTINGS) {
        move_selection_locked(direction * 4);
        return;
    }
    const int next = s_offset + direction * SR_PAGE;
    if (next < 0) return;
    if (s_total > 0 && next >= s_total) return;
    s_offset = next;
    s_sel = 0;
    s_list_top = 0;
    mark_rebuild();
    enqueue(JOB_LIST, (uint8_t)s_screen, s_offset, NULL, NULL);
}

static void handle_key_inner(bsp_btn_t btn, bsp_btn_ev_t ev);

static void handle_home_key_locked(bsp_btn_t btn, bsp_btn_ev_t ev) {
    switch (sr_home_action(btn, ev)) {
        case SR_HOME_PREV: send_command_locked(SR_CMD_PREV, 0, NULL, s_pb.device_id); break;
        case SR_HOME_NEXT: send_command_locked(SR_CMD_NEXT, 0, NULL, s_pb.device_id); break;
        case SR_HOME_TOGGLE_PLAY:
            send_command_locked(SR_CMD_PLAY_PAUSE, s_pb.playing ? 0 : 1, NULL, s_pb.device_id);
            break;
        case SR_HOME_VOLUME_UP: adjust_volume_locked(5); break;
        case SR_HOME_VOLUME_DOWN: adjust_volume_locked(-5); break;
        case SR_HOME_MENU: enter_screen_locked(SR_SC_MENU, "菜单", 0); break;
        case SR_HOME_TOGGLE_LIKE: toggle_like_locked(); break;
        default: break;
    }
}

static void handle_list_key_locked(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) move_selection_locked(-1);
        else if (btn == BSP_BTN_DOWN) move_selection_locked(1);
        else list_activate_locked();
    } else if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_UP) page_move_locked(-1);
        else if (btn == BSP_BTN_DOWN) page_move_locked(1);
        else go_back_locked();
    }
}

static void apply_brightness_locked(int value) {
    if (value < 10) value = 10;
    if (value > 100) value = 100;
    s_brightness_draft = value;
    bsp_display_backlight((uint8_t)value);
    ESP_LOGI(TAG, "亮度预览 %d%%", value);
}

static void handle_value_key_locked(bsp_btn_t btn, bsp_btn_ev_t ev) {
    const bool seek = s_screen == SR_SC_SEEK;
    const bool bright = s_screen == SR_SC_BRIGHTNESS;
    if (!seek && !bright && s_volume_draft < 0) s_volume_draft = 50;  // 设备不报音量时的合理起点
    const int step = bright ? (ev == BSP_BTN_LONG ? 20 : 10)
                            : seek ? (ev == BSP_BTN_LONG ? 30000 : 10000)
                                   : (ev == BSP_BTN_LONG ? 10 : 5);
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) {
            if (bright) {
                apply_brightness_locked(s_brightness_draft + step);
            } else if (seek) {
                s_seek_draft = s_seek_draft + step;
                if (s_pb.duration_ms > 0 && s_seek_draft > s_pb.duration_ms) s_seek_draft = s_pb.duration_ms;
            } else {
                s_volume_draft = s_volume_draft + step;
                if (s_volume_draft > 100) s_volume_draft = 100;
            }
            s_dirty = true;
        } else if (btn == BSP_BTN_DOWN) {
            if (bright) {
                apply_brightness_locked(s_brightness_draft - step);
            } else if (seek) {
                s_seek_draft = s_seek_draft - step;
                if (s_seek_draft < 0) s_seek_draft = 0;
            } else {
                s_volume_draft = s_volume_draft - step;
                if (s_volume_draft < 0) s_volume_draft = 0;
            }
            s_dirty = true;
        } else if (bright) {
            if (s_dirty) {
                s_cfg.brightness = (uint8_t)s_brightness_draft;
                s_saved_brightness = (uint8_t)s_brightness_draft;
                (void)sr_store_save_settings(&s_cfg);
                s_dirty = false;
                toast_locked("亮度已保存", false);
            } else {
                toast_locked("亮度未修改", false);
            }
            fill_settings_list();
            mark_rebuild();
        } else {
            if (s_dirty) {
                if (seek) {
                    send_command_locked(SR_CMD_SEEK, s_seek_draft, NULL, s_pb.device_id);
                } else {
                    send_command_locked(SR_CMD_VOLUME, s_volume_draft, NULL, s_pb.device_id);
                }
                s_dirty = false;
                toast_locked("已提交", false);
            } else {
                toast_locked("没有待提交的调整", false);
            }
        }
    } else if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) {
            if (bright) {
                // 取消：恢复进入页面时的亮度。
                s_brightness_draft = s_brightness_original;
                bsp_display_backlight((uint8_t)s_brightness_original);
            }
            s_dirty = false;
            go_back_locked();
            return;
        }
        if (btn == BSP_BTN_UP) {
            if (bright) {
                apply_brightness_locked(s_brightness_draft + step);
            } else if (seek) {
                s_seek_draft += step;
                if (s_pb.duration_ms > 0 && s_seek_draft > s_pb.duration_ms) s_seek_draft = s_pb.duration_ms;
            } else {
                s_volume_draft += step;
                if (s_volume_draft > 100) s_volume_draft = 100;
            }
            s_dirty = true;
        } else if (btn == BSP_BTN_DOWN) {
            if (bright) {
                apply_brightness_locked(s_brightness_draft - step);
            } else if (seek) {
                s_seek_draft -= step;
                if (s_seek_draft < 0) s_seek_draft = 0;
            } else {
                s_volume_draft -= step;
                if (s_volume_draft < 0) s_volume_draft = 0;
            }
            s_dirty = true;
        }
    }
}

static void handle_key_inner(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_screen == SR_SC_HOME) {
        handle_home_key_locked(btn, ev);
    } else if (s_screen == SR_SC_VOLUME || s_screen == SR_SC_SEEK ||
               s_screen == SR_SC_BRIGHTNESS) {
        handle_value_key_locked(btn, ev);
    } else if (s_screen == SR_SC_HELP || s_screen == SR_SC_PAIRING) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            go_back_locked();
        } else if (s_screen == SR_SC_PAIRING && ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            // 配对码过期（600 秒）后，按确定重新生成；会话有效时是空操作。
            sr_pair_begin();
            sr_utf8_ellipsis("等待电脑", s_pair_status, sizeof(s_pair_status));
        }
    } else {
        handle_list_key_locked(btn, ev);
    }
    render_locked();
}

void sr_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_mutex) return;
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_DOUBLE && ev != BSP_BTN_LONG) return;
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(400)) != pdTRUE) return;

    s_last_key_us = esp_timer_get_time();
    if (s_display_suspended) {
        // 先开面板：失败就保持息屏状态，下次按键重试唤醒，避免停在“只写 GRAM 不重开扫描”的黑屏。
        if (!panel_display_on(true)) {
            ESP_LOGW(TAG, "唤醒未完成，保持息屏并等待下次按键");
            xSemaphoreGiveRecursive(s_mutex);
            return;
        }
        s_display_suspended = false;
        s_dimmed = false;
        bsp_display_backlight((uint8_t)(s_screen == SR_SC_BRIGHTNESS ? s_brightness_draft
                                                                    : s_saved_brightness));
        ESP_LOGI(TAG, "唤醒");
        s_force_rebuild = true;
        s_next_poll_us = esp_timer_get_time();
        s_next_tick_us = 0;
        render_locked();
        xSemaphoreGiveRecursive(s_mutex);
        return;  // 唤醒的这一下只用于点亮屏幕，不执行按键动作
    }
    if (s_dimmed) {
        s_dimmed = false;
        bsp_display_backlight((uint8_t)(s_screen == SR_SC_BRIGHTNESS ? s_brightness_draft
                                                                    : s_saved_brightness));
    }
    handle_key_inner(btn, ev);
    xSemaphoreGiveRecursive(s_mutex);
}

// ---- 工作线程 ----

static void job_result_toast(const sr_result_t *result) {
    if (!result || result->text[0] == '\0') return;
    toast_locked(result->text, result->status != 1);
}

// 网络阶段不持锁：否则一次 1s 的轮询会挡住按键。结果只在加锁后短暂应用。
static void do_poll(void) {
    const int64_t start = esp_timer_get_time();
    sr_playback_t fresh;
    sr_result_t result;
    sr_playback_reset(&fresh);
    const bool ok = sr_sp_load_playback(&s_sp, &fresh, &result);

    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!ok) {
        ++s_poll_failures;
        if (sr_net_connected() && !sr_net_time_valid()) {
            toast_locked("正在等待网络校时，稍后重试", true);
        } else {
            job_result_toast(&result);
        }
        s_next_poll_us = esp_timer_get_time() + sr_poll_delay_ms(start, esp_timer_get_time(), s_sp.blocked_until_us,
                                                 false, s_poll_failures, (sr_poll_mode_t)s_cfg.poll_mode) * 1000;
        render_locked();
        xSemaphoreGiveRecursive(s_mutex);
        return;
    }
    s_poll_failures = 0;
    s_pb = fresh;
    const bool same_track = s_progress.identity == sr_hash32(s_pb.uri);
    sr_progress_update(&s_progress, sr_hash32(s_pb.uri), s_pb.position_ms, s_pb.duration_ms,
                       s_pb.playing, esp_timer_get_time());
    if (!s_dirty) s_volume_draft = s_pb.volume;
    if (!same_track) {
        s_liked = false;
        s_like_known = false;
        s_like_after_check = false;
        if (s_pb.uri[0]) enqueue(JOB_LIKE_CHECK, 0, 0, s_pb.uri, NULL);
    }
    if (strcmp(s_pb.cover_url, s_cover_url) != 0) {
        sr_utf8_ellipsis(s_pb.cover_url, s_cover_url, sizeof(s_cover_url));
        sr_cover_clear();
        s_cover_retry_us = 0;
    }
    if (s_cover_url[0] && !s_cover_pending && !sr_cover_matches(s_cover_url) &&
        esp_timer_get_time() >= s_cover_retry_us) {
        s_cover_pending = enqueue(JOB_COVER, 0, 0, s_cover_url, NULL);
        if (!s_cover_pending) s_cover_retry_us = esp_timer_get_time() + 3000000;
    }
    s_next_poll_us = esp_timer_get_time() + sr_poll_delay_ms(start, esp_timer_get_time(), s_sp.blocked_until_us,
                                             s_pb.playing, 0, (sr_poll_mode_t)s_cfg.poll_mode) * 1000;
    // 菜单停在屏幕上时也让“随机/循环”副标题跟随服务器状态。
    if (is_navigation_screen(s_screen)) fill_menu_list();
    render_locked();
    xSemaphoreGiveRecursive(s_mutex);
}

static void do_list(uint8_t screen, int offset) {
    char playlist_id[48] = "";
    char playlist_uri[80] = "";
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(300)) != pdTRUE) return;
    bool stale = s_screen != (sr_screen_t)screen || s_offset != offset;
    xSemaphoreGiveRecursive(s_mutex);
    if (stale) return;
    if (screen == SR_SC_TRACKS) {
        if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(300)) != pdTRUE) return;
        sr_utf8_ellipsis(s_playlist_id, playlist_id, sizeof(playlist_id));
        sr_utf8_ellipsis(s_playlist_uri, playlist_uri, sizeof(playlist_uri));
        xSemaphoreGiveRecursive(s_mutex);
    }

    // worker 单线程，列表缓冲放 .bss：sr_list_t 约 4.2 KB，留在栈上会把工作栈吃掉一半，
    // 而后面还要走 sp_call → sr_http_do → mbedTLS 完整握手。每次进入都 memset，避免残留上一页。
    static sr_list_t fresh;
    sr_result_t result;
    memset(&fresh, 0, sizeof(fresh));
    bool ok = false;
    if (screen == SR_SC_PLAYLISTS) {
        ok = sr_sp_load_playlists(&s_sp, offset, SR_PAGE, &fresh, &result);
    } else if (screen == SR_SC_TRACKS) {
        ok = sr_sp_load_playlist_items(&s_sp, playlist_id, playlist_uri, offset, SR_PAGE, &fresh,
                                       &result);
    } else if (screen == SR_SC_DEVICES) {
        ok = sr_sp_load_devices(&s_sp, &fresh, &result);
    } else if (screen == SR_SC_QUEUE) {
        ok = sr_sp_load_queue(&s_sp, &fresh, &result);
    } else if (screen == SR_SC_RECENT) {
        ok = sr_sp_load_recent(&s_sp, &fresh, &result);
    } else {
        return;
    }

    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    // 页面可能已切走：只在仍是同一页时应用。
    if (s_screen != (sr_screen_t)screen || s_offset != offset) {
        xSemaphoreGiveRecursive(s_mutex);
        return;
    }
    if (ok) {
        if (screen == SR_SC_TRACKS && offset == 0 && fresh.count > 0 && fresh.count < SR_LIST_MAX) {
            // 顶部插入“播放全部”，用 playlist context 从第一首开始。
            for (int i = fresh.count; i > 0; --i) fresh.items[i] = fresh.items[i - 1];
            sr_item_t *all = &fresh.items[0];
            memset(all, 0, sizeof(*all));
            sr_utf8_ellipsis("播放全部", all->title, sizeof(all->title));
            char sub[64];
            snprintf(sub, sizeof(sub), "共 %d 首 · 从第一首开始", fresh.total);
            sr_utf8_ellipsis(sub, all->subtitle, sizeof(all->subtitle));
            sr_utf8_ellipsis(playlist_uri, all->uri, sizeof(all->uri));
            all->position = -1;
            all->actionable = true;
            ++fresh.count;
        }
        s_list = fresh;
        s_total = fresh.total;
        if (screen == SR_SC_TRACKS || screen == SR_SC_PLAYLISTS) s_offset = fresh.offset;
        if (s_sel >= s_list.count) s_sel = s_list.count > 0 ? s_list.count - 1 : 0;
        if (s_sel < s_list_top) s_list_top = s_sel;
        if (s_list.count > 0 && s_sel >= s_list_top + SR_VIEW_ROWS) {
            s_list_top = s_sel - SR_VIEW_ROWS + 1;
        }
        if (result.status == 1 && s_list.count == 0) job_result_toast(&result);
    } else {
        job_result_toast(&result);
        s_total = 0;
        memset(&s_list, 0, sizeof(s_list));
    }
    mark_rebuild();
    render_locked();
    xSemaphoreGiveRecursive(s_mutex);
}

static void do_command(uint8_t type, int value, const char *text, const char *extra) {
    sr_command_t command = {0};
    command.type = (sr_cmd_t)type;
    command.value = value;
    if (text) sr_utf8_ellipsis(text, command.uri, sizeof(command.uri));
    if (extra) sr_utf8_ellipsis(extra, command.device_id, sizeof(command.device_id));

    sr_result_t result;
    const bool ok = sr_sp_command(&s_sp, &command, &result);

    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    sr_gate_release(&s_gate, esp_timer_get_time());
    if (ok) {
        if (result.text[0]) toast_locked(result.text, false);
        s_next_poll_us = esp_timer_get_time() + 250000;
    } else {
        job_result_toast(&result);
        if (command.type == SR_CMD_VOLUME) s_volume_draft = -1;
    }
    render_locked();
    xSemaphoreGiveRecursive(s_mutex);
}

static void do_like_check(const char *uri) {
    sr_result_t result;
    bool liked = false;
    const bool ok = sr_sp_liked(&s_sp, uri, &liked, &result);
    if (xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY) != pdTRUE) return;
    if (strcmp(s_pb.uri, uri) == 0) {
        if (ok) {
            s_liked = liked;
            s_like_known = true;
            if (s_like_after_check) {
                s_like_after_check = false;
                toggle_like_locked();
            }
        } else if (s_like_after_check) {
            s_like_after_check = false;
            job_result_toast(&result);
        }
        render_locked();
    }
    xSemaphoreGiveRecursive(s_mutex);
}

static void do_like_set(const char *uri, bool liked) {
    sr_result_t result;
    const bool ok = sr_sp_set_like(&s_sp, uri, liked, &result);
    if (xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY) != pdTRUE) return;
    s_like_pending = false;
    if (ok) {
        if (strcmp(s_pb.uri, uri) == 0) {
            s_liked = liked;
            s_like_known = true;
        }
        toast_locked(liked ? "已收藏" : "已取消收藏", false);
    } else {
        if (strcmp(s_pb.uri, uri) == 0) s_like_known = false;
        job_result_toast(&result);
    }
    render_locked();
    xSemaphoreGiveRecursive(s_mutex);
}

static void idle_tick_locked(void) {
    const int64_t now = esp_timer_get_time();
    if (s_auth_provisioned && !sr_pair_active()) {
        s_auth_provisioned = false;
        if (s_screen == SR_SC_PAIRING) {
            s_nav_depth = 0;
            s_screen = SR_SC_HOME;
            s_sel = 0;
            s_offset = 0;
            s_list_top = 0;
            mark_rebuild();
        }
        toast_locked("配置完成，正在同步播放状态", false);
        s_next_poll_us = now;
        render_locked();
    }
    if (s_cfg.dim_sec > 0 && !s_display_suspended && !sr_pair_active()) {
        const int64_t idle_us = now - s_last_key_us;
        if (!s_dimmed && idle_us > (int64_t)s_cfg.dim_sec * 1000000) {
            s_dimmed = true;
            bsp_display_backlight(10);
        }
        if (s_dimmed && idle_us > ((int64_t)s_cfg.dim_sec + 30) * 1000000) {
            // 息屏：关闭面板扫描与背光，重绘与同步暂停；LVGL 与 Wi-Fi 保持在线，按键立即恢复。
            sleep_now_locked();
        }
    }
    if (now > s_next_battery_us) {
        s_battery = bsp_battery_soc();
        s_next_battery_us = now + 60 * 1000000;
        render_locked();
    }
    if (!s_diag_logged && now - s_boot_us > 60 * 1000000LL) {
        s_diag_logged = true;
        log_heap("运行 60 秒");
    }
    if (s_render_pending) render_locked();
    if (esp_timer_get_time() > s_toast_until_us && s_toast[0]) {
        s_toast[0] = '\0';
        render_locked();
    }
}

static bool poll_due_locked(void) {
    // 429 封禁期内不再发请求：否则每 2 秒撞一次墙、每次都弹提示。
    // 校时完成前不发 HTTPS：证书有效期校验会直接失败，界面却显示成“网络请求失败”，
    // 很容易被误判为 Wi-Fi 故障。
    return s_screen == SR_SC_HOME && !s_display_suspended && sr_net_connected() &&
           sr_net_time_valid() && sr_store_has_auth(&s_cfg) && !s_sp.auth_failed &&
           !sr_sp_blocked(&s_sp, esp_timer_get_time()) &&
           esp_timer_get_time() >= s_next_poll_us;
}

static void worker_task(void *arg) {
    (void)arg;
    sr_job_t job;
    for (;;) {
        int64_t wait_ms = 200;
        if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            idle_tick_locked();
            const int64_t now = esp_timer_get_time();
            if (s_display_suspended) {
                // 唤醒走按键回调，不依赖 worker 周期；5s 只为处理配对写入与息屏计时。
                wait_ms = 5000;
                s_next_tick_us = 0;  // 息屏期间不累积 tick，唤醒后立刻补一帧
            }
            if (!s_display_suspended && s_screen == SR_SC_HOME) {
                if (sr_net_connected()) {
                    // 本地 UI tick：仅重绘文本与进度条，不发请求、不改 s_next_poll_us。
                    const int64_t tick_us = s_pb.playing ? SR_TICK_PLAY_US : SR_TICK_PAUSE_US;
                    if (s_next_tick_us <= now) {
                        s_next_tick_us = now + tick_us;
                        render_locked();
                    }
                    int64_t candidate;
                    if (sr_net_time_valid() && sr_store_has_auth(&s_cfg) && !s_sp.auth_failed) {
                        candidate = (s_next_poll_us - esp_timer_get_time()) / 1000;
                        if (candidate < 0) candidate = 0;
                        if (candidate < wait_ms) wait_ms = candidate;
                    }
                    candidate = (s_next_tick_us - esp_timer_get_time()) / 1000;
                    if (candidate < 0) candidate = 0;
                    if (candidate < wait_ms) wait_ms = candidate;
                } else {
                    // 离线：进度不会变，既不 tick 也不轮询；1 秒醒一次等联网事件。
                    s_next_tick_us = 0;
                    if (wait_ms < 1000) wait_ms = 1000;
                }
            }
            // 429 封禁期内 poll_due_locked() 为假，但 s_next_poll_us 已过期，上面会算成 0；
            // 不在这里抬回去就会退化成 20ms 空转（最长 15 分钟）。最多睡 5 秒，等待按键/配对事件。
            if (!s_display_suspended && sr_sp_blocked(&s_sp, esp_timer_get_time())) {
                const int64_t left_ms = sr_sp_block_left_ms(&s_sp, esp_timer_get_time());
                wait_ms = left_ms > 5000 ? 5000 : left_ms;
            }
            const bool due = poll_due_locked();
            xSemaphoreGiveRecursive(s_mutex);
            if (due) {
                do_poll();
                continue;
            }
        }
        if (wait_ms < 20) wait_ms = 20;

        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(wait_ms)) != pdTRUE) continue;
        switch (job.kind) {
            case JOB_POLL:
                do_poll();
                break;
            case JOB_COMMAND:
                do_command(job.arg, job.value, job.text, job.extra);
                break;
            case JOB_LIST:
                do_list(job.arg, job.value);
                break;
            case JOB_COVER: {
                const bool got = sr_cover_prepare(job.text);
                if (got) log_heap("封面解码后");
                if (xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY) == pdTRUE) {
                    s_cover_pending = false;
                    if (!got) s_cover_retry_us = esp_timer_get_time() + 30000000;
                    xSemaphoreGiveRecursive(s_mutex);
                }
                if (bsp_lvgl_lock(300)) {
                    sr_ui_attach_cover();
                    bsp_lvgl_unlock();
                }
                break;
            }
            case JOB_LIKE_CHECK:
                do_like_check(job.text);
                break;
            case JOB_LIKE_SET:
                do_like_set(job.text, job.value != 0);
                break;
            default:
                break;
        }
    }
}

// ---- 配对回调（运行在配对任务）----

static bool pair_on_wifi(const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    if (!ssid || !ssid[0]) return false;
    const esp_err_t err = sr_store_save_network(ssid, pass);
    if (err != ESP_OK) return false;
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    sr_utf8_ellipsis(ssid, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    sr_utf8_ellipsis(pass, s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    xSemaphoreGiveRecursive(s_mutex);
    return sr_net_start(ssid, pass) == ESP_OK;
}

static bool pair_on_auth(const char *client_id, const char *access, const char *refresh,
                         const char *scope, int64_t expires_at_ms, void *ctx) {
    (void)ctx;
    if (!client_id || !access || !refresh) return false;
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    const esp_err_t err = sr_store_save_auth(client_id, access, refresh, scope, expires_at_ms);
    if (err == ESP_OK) {
        sr_utf8_ellipsis(client_id, s_cfg.client_id, sizeof(s_cfg.client_id));
        sr_utf8_ellipsis(access, s_cfg.access_token, sizeof(s_cfg.access_token));
        sr_utf8_ellipsis(refresh, s_cfg.refresh_token, sizeof(s_cfg.refresh_token));
        sr_utf8_ellipsis(scope, s_cfg.scope, sizeof(s_cfg.scope));
        s_cfg.expires_at_ms = expires_at_ms;
        s_sp.auth_failed = false;
        s_auth_provisioned = true;
    }
    xSemaphoreGiveRecursive(s_mutex);
    return err == ESP_OK;
}

static bool pair_query_auth(void *ctx) {
    (void)ctx;
    return sr_store_has_auth(&s_cfg);
}

static void pair_on_state(const char *text, void *ctx) {
    (void)ctx;
    // 与 pair_on_auth 一致：这段文字会被 worker/按键线程读到，必须持锁写。
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        sr_utf8_ellipsis(text, s_pair_status, sizeof(s_pair_status));
        xSemaphoreGiveRecursive(s_mutex);
    }
}

static void net_event(void) {
    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        mark_rebuild();
        if (sr_net_connected()) s_next_poll_us = esp_timer_get_time();
        xSemaphoreGiveRecursive(s_mutex);
    }
}

esp_err_t sr_app_start(void) {
    s_mutex = xSemaphoreCreateRecursiveMutex();
    s_jobs = xQueueCreate(SR_JOB_DEPTH, sizeof(sr_job_t));
    if (!s_mutex || !s_jobs) return ESP_ERR_NO_MEM;

    sr_store_load(&s_cfg);
    sr_store_init();
    sr_playback_reset(&s_pb);
    sr_progress_init(&s_progress);
    sr_gate_init(&s_gate, 800000);
    sr_ui_init();
    sr_sp_init(&s_sp, &s_cfg);
    (void)sr_cover_init();

    s_saved_brightness = s_cfg.brightness;
    bsp_display_backlight(s_cfg.brightness);
    s_battery = bsp_battery_soc();
    s_last_key_us = esp_timer_get_time();
    s_boot_us = s_last_key_us;
    s_next_battery_us = s_last_key_us + 60000000;

    const sr_pair_callbacks_t callbacks = {
        .on_wifi = pair_on_wifi,
        .on_auth = pair_on_auth,
        .on_state = pair_on_state,
        .query_auth = pair_query_auth,
        .ctx = NULL,
    };
    const esp_err_t pair_err = sr_pair_start(&callbacks);
    if (pair_err != ESP_OK) ESP_LOGW(TAG, "配对串口不可用：%s", esp_err_to_name(pair_err));

    sr_net_set_event_cb(net_event);
    if (sr_store_has_wifi(&s_cfg)) {
        if (sr_net_start(s_cfg.wifi_ssid, s_cfg.wifi_pass) != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi 启动失败，界面保持离线");
        }
    }

    // 12288：do_list 的列表缓冲已移到 .bss，这里留给 sp_call/sr_http_do + mbedTLS 握手（3–6 KB）。
    if (xTaskCreate(worker_task, "sr_work", 12288, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    log_heap("启动完成");

    if (xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (!sr_store_has_wifi(&s_cfg) || !sr_store_has_auth(&s_cfg)) {
            char title[40];
            sr_utf8_ellipsis("电脑配对", title, sizeof(title));
            enter_screen_locked(SR_SC_PAIRING, title, 0);
            if (sr_store_has_wifi(&s_cfg)) {
                /* 已有 Wi-Fi 只缺授权：配对页等电脑写入令牌。 */
            }
        } else {
            s_screen = SR_SC_HOME;
            s_next_poll_us = esp_timer_get_time();
            mark_rebuild();
        }
        render_locked();
        xSemaphoreGiveRecursive(s_mutex);
    }
    return ESP_OK;
}
