// main/sr_text.h
// UTF-8 边界处理与时间格式化：字体子集有限，长文本按字节安全截断。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 下一个码位的字节数；非法首字节按 1 处理，保证不会死循环。
size_t sr_utf8_seq_len(unsigned char lead);
typedef bool (*sr_glyph_supported_fn)(uint32_t codepoint, void *context);
// Copy supported, valid UTF-8 codepoints; unsupported ones consume no space.
void sr_utf8_filter(const char *src, char *dst, size_t cap,
                    sr_glyph_supported_fn supported, void *context);
// 截断到 cap 字节以内（含结尾 0），不切断多字节字符；发生截断时末尾补省略号。
void sr_utf8_ellipsis(const char *src, char *dst, size_t cap);
// 取前 count 个码位（用于两行滚动的窗口）。
void sr_utf8_prefix(const char *src, char *dst, size_t cap, size_t count);
// 去掉首尾空白与控制字符。
void sr_utf8_trim(const char *src, char *dst, size_t cap);
// 毫秒 -> "m:ss"，非法值给 "--:--"。
void sr_format_time(int32_t ms, char *out, size_t cap);
// FNV-1a：给进度模块提供稳定 identity。
uint32_t sr_hash32(const char *text);
