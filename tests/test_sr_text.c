// tests/test_sr_text.c
// 主机测试：UTF-8 截断不切断多字节字符、时间格式与哈希稳定性。
#include "sr_text.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static bool mock_glyph(uint32_t cp, void *context) {
    (void)context;
    return cp != 0x2744 && cp != 0xfe0f && cp != 0x1f600;
}

static void check(bool condition, const char *what) {
    if (!condition) {
        ++s_failures;
        printf("FAIL: %s\n", what);
    }
}

// 校验字符串是合法 UTF-8（无孤立续字节）。
static bool valid_utf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        const size_t length = sr_utf8_seq_len(*p);
        for (size_t i = 1; i < length; ++i) {
            if ((p[i] & 0xC0U) != 0x80U) return false;
        }
        p += length;
    }
    return true;
}

int main(void) {
    char buffer[32];
    sr_utf8_filter("歌❄️名😀A", buffer, sizeof(buffer), mock_glyph, NULL);
    check(strcmp(buffer, "歌名A") == 0, "unsupported glyphs disappear without spaces");
    sr_utf8_filter("歌😀名", buffer, 4, mock_glyph, NULL);
    check(strcmp(buffer, "歌") == 0, "filter respects UTF-8 output boundaries");
    sr_utf8_filter("a\xff" "b", buffer, sizeof(buffer), mock_glyph, NULL);
    check(strcmp(buffer, "ab") == 0, "filter skips invalid UTF-8");
    const char *chinese = "正在播放一首名字非常长的歌曲标题，需要截断";
    sr_utf8_ellipsis(chinese, buffer, 20);
    check(valid_utf8(buffer), "截断结果是合法 UTF-8");
    check(strstr(buffer, "\xE2\x80\xA6") != NULL, "截断补省略号");
    check(strlen(buffer) <= 19, "截断不超过容量");

    sr_utf8_ellipsis("短标题", buffer, sizeof(buffer));
    check(strcmp(buffer, "短标题") == 0, "短文本原样复制");

    sr_utf8_ellipsis(NULL, buffer, sizeof(buffer));
    check(buffer[0] == '\0', "空指针安全");

    sr_utf8_prefix("一二三四五", buffer, sizeof(buffer), 3);
    check(strcmp(buffer, "一二三") == 0, "按码位取前缀");

    sr_utf8_trim("  标题  ", buffer, sizeof(buffer));
    check(strcmp(buffer, "标题") == 0, "去首尾空白");

    // 容量边界：整字 3 字节，绝不能只截半个汉字。
    sr_utf8_ellipsis("一二三", buffer, 7);
    check(strcmp(buffer, "一\xE2\x80\xA6") == 0, "cap=7 保留一个整字加省略号");
    check(valid_utf8(buffer), "cap=7 结果是合法 UTF-8");
    sr_utf8_ellipsis("一二三", buffer, 6);
    check(strcmp(buffer, "\xE2\x80\xA6") == 0, "cap=6 不足以放整字时只留省略号");
    check(valid_utf8(buffer), "cap=6 结果是合法 UTF-8");
    sr_utf8_ellipsis("一二三", buffer, 5);
    check(valid_utf8(buffer), "cap=5 结果是合法 UTF-8");
    check(strlen(buffer) <= 4, "cap=5 不超过容量");
    // ASCII 混合：截断点落在 ASCII 与汉字之间也要正确。
    sr_utf8_ellipsis("Spotify 音乐", buffer, 11);
    check(valid_utf8(buffer), "混合文本截断合法");
    check(strncmp(buffer, "Spotify", 7) == 0, "混合文本保留 ASCII 前缀");

    sr_utf8_prefix("一二三四五", buffer, 4, 5);
    check(valid_utf8(buffer), "前缀受容量限制时仍合法");
    check(strcmp(buffer, "一") == 0, "前缀不越过容量写半个字");

    char clock[16];
    sr_format_time(0, clock, sizeof(clock));
    check(strcmp(clock, "0:00") == 0, "0 毫秒");
    sr_format_time(185000, clock, sizeof(clock));
    check(strcmp(clock, "3:05") == 0, "3 分 05 秒");
    sr_format_time(-1, clock, sizeof(clock));
    check(strcmp(clock, "--:--") == 0, "非法时间");

    check(sr_hash32("spotify:track:abc") == sr_hash32("spotify:track:abc"), "哈希稳定");
    check(sr_hash32("a") != sr_hash32("b"), "不同输入不同哈希");
    check(sr_hash32("") != 0, "空串哈希非零");

    if (s_failures) {
        printf("test_sr_text: %d 项失败\n", s_failures);
        return 1;
    }
    printf("test_sr_text: PASS\n");
    return 0;
}
