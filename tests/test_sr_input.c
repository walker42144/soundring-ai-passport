#include "sr_input.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(sr_home_action(BSP_BTN_OK, BSP_BTN_CLICK) == SR_HOME_TOGGLE_PLAY);
    assert(sr_home_action(BSP_BTN_OK, BSP_BTN_DOUBLE) == SR_HOME_TOGGLE_LIKE);
    assert(sr_home_action(BSP_BTN_OK, BSP_BTN_LONG) == SR_HOME_MENU);
    assert(sr_home_action(BSP_BTN_UP, BSP_BTN_DOUBLE) == SR_HOME_STATUS);
    assert(sr_home_action(BSP_BTN_DOWN, BSP_BTN_DOUBLE) == SR_HOME_IMMERSIVE);
    assert(sr_home_action(BSP_BTN_UP, BSP_BTN_CLICK) == SR_HOME_PREV);
    assert(sr_home_action(BSP_BTN_DOWN, BSP_BTN_CLICK) == SR_HOME_NEXT);
    puts("home button mapping PASS");
    return 0;
}
