#include "sr_ui.h"
#include "lvgl.h"
#include "sr_cover.h"
#include "sr_text.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(sr_font_14);
LV_FONT_DECLARE(sr_font_20);
LV_FONT_DECLARE(sr_font_28);

/* SoundRing: an information-first, three-button interface for a 240x320 screen. */
enum {
    BG = 0x080a09,
    CARD = 0x171b19,
    CARD_ACTIVE = 0x283b2e,
    TEXT = 0xf4f6f4,
    MUTED = 0xa8b0ab,
    ACCENT = 0x1ed760,
    LINE = 0x343b36,
    RED = 0xff7777
};
static lv_obj_t *root, *status, *footer, *toast;
static lv_obj_t *art, *art_placeholder, *title, *artist, *device, *bar, *elapsed, *duration, *flags,
    *playing;
static lv_obj_t *page, *rows[SR_VIEW_ROWS], *row_title[SR_VIEW_ROWS], *row_sub[SR_VIEW_ROWS],
    *row_mark[SR_VIEW_ROWS];
static lv_obj_t *value, *value_sub, *value_hint, *value_bar;
static lv_image_dsc_t cover_desc;
static sr_screen_t current = (sr_screen_t)-1;
static sr_progress_t progress_snapshot;
static lv_timer_t *progress_timer;

static bool font_has_glyph(uint32_t cp, void *context) {
    lv_font_glyph_dsc_t glyph = {0};
    const lv_font_t *font = context;
    return lv_font_get_glyph_dsc(font, &glyph, cp, 0) && !glyph.is_placeholder;
}

static void set_metadata(lv_obj_t *label, const char *value, const lv_font_t *font) {
    char filtered[512];
    sr_utf8_filter(value, filtered, sizeof(filtered), font_has_glyph, (void *)font);
    lv_label_set_text(label, filtered);
}

static void progress_tick(lv_timer_t *timer) {
    (void)timer;
    if (current != SR_SC_HOME || !bar || !elapsed || !progress_snapshot.initialized) return;
    int32_t pos = sr_progress_position(&progress_snapshot, esp_timer_get_time());
    int32_t total = (int32_t)progress_snapshot.duration_ms;
    if (total <= 0) return;
    lv_bar_set_value(bar, (int)((int64_t)pos * 1000 / total), LV_ANIM_OFF);
    char buf[24];
    sr_format_time(pos, buf, sizeof(buf));
    lv_label_set_text(elapsed, buf);
}

void sr_ui_pause_progress(void) {
    if (progress_timer) lv_timer_pause(progress_timer);
}

static void reset_refs(void) {
    status = footer = toast = art = art_placeholder = title = artist = device = bar = elapsed =
        duration = flags = playing = NULL;
    page = value = value_sub = value_hint = value_bar = NULL;
    memset(rows, 0, sizeof(rows));
    memset(row_title, 0, sizeof(row_title));
    memset(row_sub, 0, sizeof(row_sub));
    memset(row_mark, 0, sizeof(row_mark));
}
static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
static lv_obj_t *text(lv_obj_t *p, const char *str, const lv_font_t *font, uint32_t color, int x,
                      int y, int w, int h, lv_text_align_t align) {
    lv_obj_t *o = lv_label_create(p);
    lv_label_set_text(o, str ? str : "");
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(o, align, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    return o;
}
static void show(lv_obj_t *o, bool visible) {
    if (!o)
        return;
    if (visible)
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *progress(lv_obj_t *p, int x, int y, int w) {
    lv_obj_t *o = lv_bar_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 5);
    lv_bar_set_range(o, 0, 1000);
    lv_obj_set_style_bg_color(o, lv_color_hex(LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(o, 4, LV_PART_INDICATOR);
    return o;
}
static bool is_list(sr_screen_t s) {
    return s == SR_SC_MENU || s == SR_SC_LIBRARY || s == SR_SC_CONTROLS || s == SR_SC_PLAYLISTS ||
           s == SR_SC_TRACKS || s == SR_SC_DEVICES || s == SR_SC_QUEUE || s == SR_SC_RECENT ||
           s == SR_SC_SETTINGS;
}
static const char *state(const sr_view_t *v) {
    if (!v->connected)
        return "Wi-Fi 未连接";
    if (!v->time_valid)
        return "正在校时";
    if (!v->auth_ok || v->auth_failed)
        return "需要授权";
    return "已连接";
}
static const char *home_hint(const sr_view_t *v) {
    if (!v->connected)
        return "请连接 Wi-Fi · 菜单 → 设置";
    if (!v->time_valid)
        return "正在校时，请稍候";
    if (!v->auth_ok || v->auth_failed)
        return "授权失效，请在电脑重新授权";
    if (!v->playback || !v->playback->has_device)
        return "请在 Spotify 开始播放";
    return "双击收藏 · 长按确定菜单";
}
static void common(void) {
    text(root, "SOUNDRING", &lv_font_montserrat_14, ACCENT, 14, 5, 100, 18, LV_TEXT_ALIGN_LEFT);
    status = text(root, "", &sr_font_14, MUTED, 115, 4, 111, 18, LV_TEXT_ALIGN_RIGHT);
    footer = text(root, "", &sr_font_14, MUTED, 8, 300, 224, 18, LV_TEXT_ALIGN_CENTER);
    toast = text(root, "", &sr_font_14, ACCENT, 8, 300, 224, 18, LV_TEXT_ALIGN_CENTER);
    show(toast, false);
}
static void home(const sr_view_t *v) {
    (void)v;
    art_placeholder = box(root, 68, 36, 104, 104, CARD, 9);
    text(art_placeholder, LV_SYMBOL_AUDIO, &lv_font_montserrat_14, MUTED, 0, 43, 104, 18,
         LV_TEXT_ALIGN_CENTER);
    art = lv_image_create(root);
    show(art, false);
    title = text(root, "", &sr_font_20, TEXT, 16, 144, 208, 52, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    artist = text(root, "", &sr_font_14, MUTED, 16, 197, 208, 18, LV_TEXT_ALIGN_CENTER);
    device = text(root, "", &sr_font_14, ACCENT, 16, 217, 208, 18, LV_TEXT_ALIGN_CENTER);
    bar = progress(root, 18, 239, 204);
    elapsed =
        text(root, "0:00", &lv_font_montserrat_14, MUTED, 18, 249, 75, 18, LV_TEXT_ALIGN_LEFT);
    duration =
        text(root, "0:00", &lv_font_montserrat_14, MUTED, 147, 249, 75, 18, LV_TEXT_ALIGN_RIGHT);
    flags = text(root, "", &sr_font_14, MUTED, 14, 271, 212, 18, LV_TEXT_ALIGN_CENTER);
    playing = text(root, LV_SYMBOL_PLAY, &lv_font_montserrat_14, ACCENT, 102, 250, 36, 18,
                   LV_TEXT_ALIGN_CENTER);
}
static void list(const sr_view_t *v) {
    lv_obj_t *heading = text(root, "", &sr_font_20, TEXT, 14, 28, 160, 26, LV_TEXT_ALIGN_LEFT);
    set_metadata(heading, v->title, &sr_font_20);
    page = text(root, "", &lv_font_montserrat_14, MUTED, 175, 32, 50, 18, LV_TEXT_ALIGN_RIGHT);
    for (int i = 0; i < SR_VIEW_ROWS; i++) {
        rows[i] = box(root, 10, 57 + i * 39, 220, 36, CARD, 6);
        row_mark[i] = box(rows[i], 0, 5, 3, 26, ACCENT, 2);
        row_title[i] = text(rows[i], "", &sr_font_14, TEXT, 12, 0, 190, 18, LV_TEXT_ALIGN_LEFT);
        row_sub[i] = text(rows[i], "", &sr_font_14, MUTED, 12, 18, 190, 18, LV_TEXT_ALIGN_LEFT);
    }
}
static void adjustment(const char *name) {
    text(root, name, &sr_font_20, TEXT, 16, 32, 208, 26, LV_TEXT_ALIGN_LEFT);
    value = text(root, "--", &sr_font_28, ACCENT, 12, 83, 216, 40, LV_TEXT_ALIGN_CENTER);
    value_sub = text(root, "", &sr_font_14, MUTED, 16, 130, 208, 18, LV_TEXT_ALIGN_CENTER);
    value_bar = progress(root, 24, 164, 192);
    value_hint = text(root, "", &sr_font_14, TEXT, 18, 189, 204, 68, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(value_hint, LV_LABEL_LONG_WRAP);
}
static void pairing(void) {
    text(root, "连接电脑", &sr_font_20, TEXT, 16, 33, 208, 26, LV_TEXT_ALIGN_LEFT);
    text(root, "在配对中心输入此码", &sr_font_14, MUTED, 16, 66, 208, 18, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *card = box(root, 22, 95, 196, 65, CARD, 10);
    value = text(card, "--------", &sr_font_28, ACCENT, 0, 13, 196, 40, LV_TEXT_ALIGN_CENTER);
    value_sub = text(root, "", &sr_font_14, TEXT, 14, 175, 212, 18, LV_TEXT_ALIGN_CENTER);
    value_hint = text(root, "", &sr_font_14, MUTED, 14, 201, 212, 70, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(value_hint, LV_LABEL_LONG_WRAP);
}
static void help(void) {
    text(root, "按键说明", &sr_font_20, TEXT, 16, 32, 208, 26, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *b = text(
        root,
        "首页\n上 / 下：上一首 / 下一首\n确定：播放或暂停\n双击确定：收藏 / 取消\n长按上 / "
        "下：音量\n长按确定：打开菜单\n\n列表\n上 / 下：移动\n确定：打开或执行\n长按确定：返回",
        &sr_font_14, TEXT, 18, 66, 204, 218, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(b, 3, 0);
}
void sr_ui_init(void) { reset_refs(); }
void sr_ui_set_toast_color(bool error) {
    if (toast)
        lv_obj_set_style_text_color(toast, lv_color_hex(error ? RED : ACCENT), 0);
}
void sr_ui_attach_cover(void) {
    if (!art)
        return;
    uint16_t w = 0, h = 0;
    const uint8_t *pixels = sr_cover_pixels(&w, &h);
    if (!pixels || !w || !h) {
        show(art, false);
        show(art_placeholder, true);
        return;
    }
    memset(&cover_desc, 0, sizeof(cover_desc));
    cover_desc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cover_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    cover_desc.header.w = w;
    cover_desc.header.h = h;
    cover_desc.header.stride = (uint32_t)w * 2;
    cover_desc.data_size = (uint32_t)w * h * 2;
    cover_desc.data = pixels;
    lv_image_set_src(art, &cover_desc);
    lv_obj_set_pos(art, 60 + (120 - (int)w) / 2, 28 + (120 - (int)h) / 2);
    show(art, true);
    show(art_placeholder, false);
}
void sr_ui_render(const sr_view_t *v, bool rebuild) {
    if (!v)
        return;
    if (!progress_timer) progress_timer = lv_timer_create(progress_tick, 150, NULL);
    if (v->progress) progress_snapshot = *v->progress;
    if (progress_timer) {
        if (v->screen == SR_SC_HOME && v->connected &&
            sr_progress_needs_frames(&progress_snapshot, esp_timer_get_time()))
            lv_timer_resume(progress_timer);
        else
            lv_timer_pause(progress_timer);
    }
    if (rebuild || current != v->screen || !root) {
        if (root)
            lv_obj_delete(root);
        reset_refs();
        root = box(lv_screen_active(), 0, 0, 240, 320, BG, 0);
        common();
        if (v->screen == SR_SC_HOME)
            home(v);
        else if (is_list(v->screen))
            list(v);
        else if (v->screen == SR_SC_PAIRING)
            pairing();
        else if (v->screen == SR_SC_HELP)
            help();
        else if (v->screen == SR_SC_VOLUME)
            adjustment("音量");
        else if (v->screen == SR_SC_SEEK)
            adjustment("播放进度");
        else if (v->screen == SR_SC_BRIGHTNESS)
            adjustment("屏幕亮度");
        current = v->screen;
    }
    char buf[160];
    if (v->battery >= 0)
        snprintf(buf, sizeof(buf), "%s  %d%%", state(v), v->battery);
    else
        snprintf(buf, sizeof(buf), "%s", state(v));
    lv_label_set_text(status, buf);
    const char *hint = "长按确定 返回";
    if (v->screen == SR_SC_HOME) {
        const sr_playback_t *p = v->playback;
        set_metadata(title, p && p->title[0] ? p->title : "还没有播放", &sr_font_20);
        set_metadata(artist, p && p->artist[0] ? p->artist : "打开 Spotify 开始播放", &sr_font_14);
        snprintf(buf, sizeof(buf), "%s", p && p->has_device ? p->device_name : "没有播放设备");
        set_metadata(device, buf, &sr_font_14);
        int total = p && p->duration_ms > 0 ? p->duration_ms : 1;
        int progress_value = (int)((int64_t)v->position_ms * 1000 / total);
        if (progress_value < 0)
            progress_value = 0;
        if (progress_value > 1000)
            progress_value = 1000;
        lv_bar_set_value(bar, progress_value, LV_ANIM_OFF);
        sr_format_time(v->position_ms, buf, sizeof(buf));
        lv_label_set_text(elapsed, buf);
        sr_format_time(p ? p->duration_ms : -1, buf, sizeof(buf));
        lv_label_set_text(duration, buf);
        lv_label_set_text(playing, p && p->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        snprintf(buf, sizeof(buf), "%s   %s   %s", v->like_known ? (v->liked ? "已收藏" : "未收藏") : "收藏同步中",
                 p && p->shuffle ? "随机开" : "随机关",
                 p && strcmp(p->repeat, "off") ? "循环开" : "循环关");
        lv_label_set_text(flags, buf);
        sr_ui_attach_cover();
        hint = home_hint(v);
    } else if (is_list(v->screen)) {
        snprintf(buf, sizeof(buf), "%d/%d", v->page, v->pages);
        lv_label_set_text(page, buf);
        for (int i = 0; i < SR_VIEW_ROWS; i++) {
            int n = v->list_top + i;
            bool exists = v->list && n >= 0 && n < v->list->count;
            show(rows[i], exists);
            if (!exists)
                continue;
            bool selected = n == v->sel;
            set_metadata(row_title[i], v->list->items[n].title, &sr_font_14);
            set_metadata(row_sub[i], v->list->items[n].subtitle, &sr_font_14);
            lv_obj_set_style_bg_color(rows[i], lv_color_hex(selected ? CARD_ACTIVE : CARD), 0);
            lv_obj_set_style_text_color(row_title[i], lv_color_hex(selected ? ACCENT : TEXT), 0);
            show(row_mark[i], selected);
        }
        hint = "上下选择  确定打开  长按返回";
    } else if (v->screen == SR_SC_PAIRING) {
        lv_label_set_text(value, v->pairing_active && v->pair_code ? v->pair_code : "--------");
        lv_label_set_text(value_sub, v->pairing_active
                                         ? (v->pair_status ? v->pair_status : "等待电脑")
                                         : "配对已过期");
        if (v->pairing_active)
            snprintf(buf, sizeof(buf), "剩余 %d 秒\n保持 USB 连接\n在电脑完成 Wi-Fi 和授权",
                     v->pair_seconds);
        else
            snprintf(buf, sizeof(buf), "按确定重新生成配对码");
        lv_label_set_text(value_hint, buf);
    } else if (value && v->screen != SR_SC_PAIRING) {
        if (v->screen == SR_SC_SEEK)
            sr_format_time(v->draft, buf, sizeof(buf));
        else
            snprintf(buf, sizeof(buf), "%d%%", v->draft);
        lv_label_set_text(value, buf);
        lv_label_set_text(value_sub, v->screen == SR_SC_VOLUME ? "播放设备音量"
                                     : v->screen == SR_SC_SEEK ? "选择位置"
                                                               : "实时预览");
        int pct = v->screen == SR_SC_SEEK
                      ? (v->playback && v->playback->duration_ms > 0
                             ? (int)((int64_t)v->draft * 1000 / v->playback->duration_ms)
                             : 0)
                      : v->draft * 10;
        if (pct < 0)
            pct = 0;
        if (pct > 1000)
            pct = 1000;
        lv_bar_set_value(value_bar, pct, LV_ANIM_OFF);
        lv_label_set_text(value_hint, v->dirty ? "已调整 · 按确定保存" : "上下调整 · 长按加速");
    }
    bool has = v->toast && v->toast[0];
    lv_label_set_text(footer, hint);
    lv_label_set_text(toast, has ? v->toast : "");
    show(footer, !has);
    show(toast, has);
}
