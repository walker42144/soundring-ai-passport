#include "sr_navigation.h"
static const sr_nav_item_t root[] = {
    {"音乐库", "歌单 · 最近播放 · 队列", SR_NAV_MUSIC},
    {"播放设备", "选择 Spotify 播放设备", SR_NAV_DEVICES},
    {"播放控制", "音量 · 进度 · 收藏", SR_NAV_CONTROL},
    {"设置", "配对 · 显示 · 省电", SR_NAV_SETTINGS},
    {"立即息屏", "任意键唤醒", SR_NAV_SLEEP},
};
static const sr_nav_item_t library[] = {
    {"歌单", "浏览并播放", SR_NAV_PLAYLISTS},
    {"最近播放", "继续最近听过的音乐", SR_NAV_RECENT},
    {"播放队列", "查看接下来播放的内容", SR_NAV_QUEUE},
};
static const sr_nav_item_t controls[] = {
    {"音量", "调整播放设备音量", SR_NAV_VOLUME},
    {"播放进度", "跳转到指定位置", SR_NAV_SEEK},
    {"收藏当前", "收藏或取消收藏", SR_NAV_LIKE},
    {"随机播放", "", SR_NAV_SHUFFLE},
    {"循环模式", "", SR_NAV_REPEAT},
};
size_t sr_nav_count(sr_nav_group_t g) {
    switch (g) {
    case SR_NAV_ROOT:
        return sizeof(root) / sizeof(root[0]);
    case SR_NAV_LIBRARY:
        return sizeof(library) / sizeof(library[0]);
    case SR_NAV_CONTROLS:
        return sizeof(controls) / sizeof(controls[0]);
    default:
        return 0;
    }
}
const sr_nav_item_t *sr_nav_item(sr_nav_group_t g, size_t i) {
    if (i >= sr_nav_count(g))
        return NULL;
    switch (g) {
    case SR_NAV_ROOT:
        return &root[i];
    case SR_NAV_LIBRARY:
        return &library[i];
    case SR_NAV_CONTROLS:
        return &controls[i];
    default:
        return NULL;
    }
}
