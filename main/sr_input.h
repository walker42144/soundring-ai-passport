#pragma once
#include "bsp_button.h"
typedef enum {
    SR_HOME_NONE,
    SR_HOME_PREV,
    SR_HOME_NEXT,
    SR_HOME_TOGGLE_PLAY,
    SR_HOME_VOLUME_UP,
    SR_HOME_VOLUME_DOWN,
    SR_HOME_MENU,
    SR_HOME_IMMERSIVE,
    SR_HOME_STATUS,
    SR_HOME_TOGGLE_LIKE
} sr_home_action_t;
sr_home_action_t sr_home_action(bsp_btn_t button, bsp_btn_ev_t event);
