#include "sr_cover_scale.h"

bool sr_cover_upscale_rgb565(uint16_t *pixels, uint16_t *width, uint16_t *height,
                             uint16_t max_dimension, size_t pixel_capacity) {
    if (!pixels || !width || !height || !*width || !*height || !max_dimension)
        return false;
    const uint32_t old_w = *width, old_h = *height;
    const uint32_t longest = old_w > old_h ? old_w : old_h;
    if ((size_t)old_w * old_h > pixel_capacity || longest > max_dimension)
        return false;
    if (longest == max_dimension)
        return true;
    const uint32_t new_w = (old_w * max_dimension + longest / 2) / longest;
    const uint32_t new_h = (old_h * max_dimension + longest / 2) / longest;
    if ((size_t)new_w * new_h > pixel_capacity)
        return false;
    // Backward traversal prevents expanded pixels overwriting unread source pixels.
    for (uint32_t y = new_h; y-- > 0;) {
        const uint32_t sy = y * old_h / new_h;
        for (uint32_t x = new_w; x-- > 0;) {
            pixels[(size_t)y * new_w + x] = pixels[(size_t)sy * old_w + x * old_w / new_w];
        }
    }
    *width = (uint16_t)new_w;
    *height = (uint16_t)new_h;
    return true;
}
