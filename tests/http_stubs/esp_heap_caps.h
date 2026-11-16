#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT 1
static inline size_t heap_caps_get_free_size(int cap) { (void)cap; return 6000; }
static inline size_t heap_caps_get_largest_free_block(int cap) { (void)cap; return 5000; }
