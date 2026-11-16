// main/sr_cover.h
// 有界封面：限长下载、按需缩放解码、RAM 单张 + SPIFFS 有界缓存。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SR_COVER_MAX_DIM 120
// 首页封面显示区就是 120x120，解码到 120px 已无缩放损失；下载上限 24KB，
// 因为 Wi-Fi 常驻后内部堆只剩几十 KB，TLS 握手还需要 25–30KB 连续空间。
#define SR_COVER_MAX_BYTES (24 * 1024)

esp_err_t sr_cover_init(void);
bool sr_cover_matches(const char *url);
// 阻塞下载/解码（只在工作线程调用）。失败时保留占位，不影响遥控。
bool sr_cover_prepare(const char *url);
// 供 LVGL 使用；未就绪返回 NULL。
const uint8_t *sr_cover_pixels(uint16_t *width, uint16_t *height);
void sr_cover_clear(void);
