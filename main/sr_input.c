#include "sr_input.h"
sr_home_action_t sr_home_action(bsp_btn_t button, bsp_btn_ev_t event) {
    if (event == BSP_BTN_CLICK) {
        if (button == BSP_BTN_UP)
            return SR_HOME_PREV;
        if (button == BSP_BTN_DOWN)
            return SR_HOME_NEXT;
        if (button == BSP_BTN_OK)
            return SR_HOME_TOGGLE_PLAY;
    }
    if (event == BSP_BTN_DOUBLE && button == BSP_BTN_OK)
        return SR_HOME_TOGGLE_LIKE;
    if (event == BSP_BTN_LONG) {
        if (button == BSP_BTN_UP)
            return SR_HOME_VOLUME_UP;
        if (button == BSP_BTN_DOWN)
            return SR_HOME_VOLUME_DOWN;
        if (button == BSP_BTN_OK)
            return SR_HOME_MENU;
    }
    return SR_HOME_NONE;
}
