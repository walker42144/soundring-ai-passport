#include "sr_text.h"

#include <stdio.h>
#include <string.h>

static const char k_ellipsis[] = "\xE2\x80\xA6";  // U+2026

size_t sr_utf8_seq_len(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;
}

void sr_utf8_filter(const char *src, char *dst, size_t cap,
                    sr_glyph_supported_fn supported, void *context) {
    if (!dst || !cap) return;
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p) {
        const size_t n = sr_utf8_seq_len(*p);
        uint32_t cp = n == 1 ? *p : (*p & (0x7fU >> n));
        bool valid = n > 1 || *p < 0x80;
        size_t i = 1;
        for (; i < n; ++i) {
            if (!p[i] || (p[i] & 0xc0U) != 0x80U) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3fU);
        }
        if (valid && ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
                      (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
                      (cp >= 0xd800 && cp <= 0xdfff))) valid = false;
        if (valid && (!supported || supported(cp, context))) {
            if (used + n >= cap) break;
            memcpy(dst + used, p, n);
            used += n;
        }
        p += valid ? n : 1;
    }
    dst[used] = '\0';
}

void sr_utf8_ellipsis(const char *src, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    const size_t length = strlen(src);
    if (length < cap) {
        memcpy(dst, src, length + 1);
        return;
    }
    const size_t ellipsis_len = sizeof(k_ellipsis) - 1;
    if (cap <= ellipsis_len) {
        memcpy(dst, k_ellipsis, cap - 1);
        dst[cap - 1] = '\0';
        return;
    }
    size_t limit = cap - 1 - ellipsis_len;
    size_t cut = 0;
    while (cut < limit) {
        const size_t step = sr_utf8_seq_len((unsigned char)src[cut]);
        if (cut + step > limit) break;
        cut += step;
    }
    memcpy(dst, src, cut);
    memcpy(dst + cut, k_ellipsis, ellipsis_len);
    dst[cut + ellipsis_len] = '\0';
}

void sr_utf8_prefix(const char *src, char *dst, size_t cap, size_t count) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t used = 0;
    size_t index = 0;
    while (src[used] && index < count) {
        const size_t step = sr_utf8_seq_len((unsigned char)src[used]);
        if (used + step >= cap) break;
        used += step;
        ++index;
    }
    memcpy(dst, src, used);
    dst[used] = '\0';
}

void sr_utf8_trim(const char *src, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    const char *begin = src;
    while (*begin && (unsigned char)*begin <= 0x20) ++begin;
    const char *end = begin + strlen(begin);
    while (end > begin && (unsigned char)end[-1] <= 0x20) --end;
    size_t length = (size_t)(end - begin);
    if (length >= cap) {
        sr_utf8_ellipsis(begin, dst, cap);
        return;
    }
    memcpy(dst, begin, length);
    dst[length] = '\0';
}

void sr_format_time(int32_t ms, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (ms < 0) {
        snprintf(out, cap, "--:--");
        return;
    }
    const int32_t total_seconds = ms / 1000;
    const int32_t minutes = total_seconds / 60;
    const int32_t seconds = total_seconds % 60;
    snprintf(out, cap, "%d:%02d", (int)minutes, (int)seconds);
}

uint32_t sr_hash32(const char *text) {
    uint32_t hash = 2166136261U;
    if (!text) return hash;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        hash ^= *p;
        hash *= 16777619U;
    }
    return hash ? hash : 1U;
}
