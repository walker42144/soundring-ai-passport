#include "sr_navigation.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(sr_nav_count(SR_NAV_ROOT) == 5);
    assert(sr_nav_item(SR_NAV_ROOT, 0)->action == SR_NAV_MUSIC);
    assert(sr_nav_item(SR_NAV_ROOT, 1)->action == SR_NAV_DEVICES);
    assert(sr_nav_item(SR_NAV_LIBRARY, 0)->action == SR_NAV_PLAYLISTS);
    assert(sr_nav_item(SR_NAV_CONTROLS, 2)->action == SR_NAV_LIKE);
    assert(sr_nav_item(SR_NAV_CONTROLS, 5) == NULL);
    assert(sr_nav_count((sr_nav_group_t)99) == 0);
    puts("navigation PASS");
    return 0;
}
