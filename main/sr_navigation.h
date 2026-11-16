#pragma once
#include <stddef.h>

typedef enum { SR_NAV_ROOT, SR_NAV_LIBRARY, SR_NAV_CONTROLS } sr_nav_group_t;
typedef enum {
    SR_NAV_MUSIC,
    SR_NAV_DEVICES,
    SR_NAV_CONTROL,
    SR_NAV_SETTINGS,
    SR_NAV_SLEEP,
    SR_NAV_PLAYLISTS,
    SR_NAV_RECENT,
    SR_NAV_QUEUE,
    SR_NAV_VOLUME,
    SR_NAV_SEEK,
    SR_NAV_SHUFFLE,
    SR_NAV_REPEAT,
    SR_NAV_LIKE
} sr_nav_action_t;
typedef struct {
    const char *title;
    const char *subtitle;
    sr_nav_action_t action;
} sr_nav_item_t;
size_t sr_nav_count(sr_nav_group_t group);
const sr_nav_item_t *sr_nav_item(sr_nav_group_t group, size_t index);
