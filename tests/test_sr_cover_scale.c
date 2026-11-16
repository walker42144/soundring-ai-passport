#include "sr_cover_scale.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint16_t pixels[16] = {1, 2, 3, 4};
    uint16_t w = 2, h = 2;
    assert(sr_cover_upscale_rgb565(pixels, &w, &h, 4, 16));
    assert(w == 4 && h == 4 && pixels[0] == 1 && pixels[3] == 2 && pixels[12] == 3 &&
           pixels[15] == 4);
    uint16_t too_small[4] = {1, 2, 3, 4};
    w = 2;
    h = 2;
    assert(!sr_cover_upscale_rgb565(too_small, &w, &h, 4, 4));
    assert(w == 2 && h == 2 && too_small[3] == 4);
    uint16_t rectangle[32] = {1, 2};
    w = 2;
    h = 1;
    assert(sr_cover_upscale_rgb565(rectangle, &w, &h, 8, 32));
    assert(w == 8 && h == 4 && rectangle[0] == 1 && rectangle[7] == 2);
    puts("cover scale PASS");
    return 0;
}
