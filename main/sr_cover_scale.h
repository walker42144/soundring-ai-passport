#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool sr_cover_upscale_rgb565(uint16_t *pixels, uint16_t *width, uint16_t *height,
                             uint16_t max_dimension, size_t pixel_capacity);
