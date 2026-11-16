#include "sr_queue_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool parse(const char *s, size_t chunk, sr_list_t *out) {
    sr_queue_stream_t r;
    sr_queue_stream_init(&r, out);
    size_t len = strlen(s);
    for (size_t i = 0; i < len; i += chunk) {
        size_t n = len - i < chunk ? len - i : chunk;
        if (!sr_queue_stream_feed(s + i, n, &r)) return false;
    }
    return sr_queue_stream_finish(&r);
}

static bool parse_recent(const char *s, size_t chunk, sr_list_t *out) {
    sr_queue_stream_t r;
    sr_recent_stream_init(&r, out);
    size_t len = strlen(s);
    for (size_t i = 0; i < len; i += chunk) {
        size_t n = len - i < chunk ? len - i : chunk;
        if (!sr_queue_stream_feed(s + i, n, &r)) return false;
    }
    return sr_queue_stream_finish(&r);
}

int main(void) {
    const char *sample = "{\"currently_playing\":{\"album\":{\"name\":\"wrong\"},"
        "\"name\":\"当前\\u6b4c\",\"uri\":\"spotify:track:a\","
        "\"artists\":[{\"name\":\"歌手甲\"},{\"name\":\"歌手乙\"}]},"
        "\"queue\":[{\"name\":\"下一首\",\"uri\":\"spotify:track:b\","
        "\"artists\":[{\"name\":\"歌手\"}],\"ignored\":[null,true,123]}]}";
    sr_list_t out;
    for (size_t chunk = 1; chunk <= 64; ++chunk) {
        assert(parse(sample, chunk, &out));
        assert(out.count == 2 && out.total == 2);
        assert(strcmp(out.items[0].title, "当前歌") == 0);
        assert(strcmp(out.items[0].subtitle, "歌手甲 / 歌手乙") == 0);
        assert(strcmp(out.items[1].title, "下一首") == 0);
        assert(strcmp(out.items[1].uri, "spotify:track:b") == 0);
    }
    assert(parse("{\"currently_playing\":null,\"queue\":[]}", 1, &out) && out.count == 0);
    assert(!parse("{\"queue\":[", 3, &out));
    assert(!parse("{\"queue\":[1,]}", 3, &out));
    assert(!parse("{\"queue\":[nonsense]}", 3, &out));
    assert(!parse("{\"not_queue\":[]}", 3, &out));
    char big[70000];
    size_t n = (size_t)snprintf(big, sizeof(big), "{\"queue\":[");
    for (int i = 0; i < 100; ++i) {
        n += (size_t)snprintf(big + n, sizeof(big) - n,
            "%s{\"name\":\"song%d\",\"album\":{\"unused\":\"%0500d\"},\"artists\":[]}",
            i ? "," : "", i, i);
    }
    snprintf(big + n, sizeof(big) - n, "]}");
    assert(strlen(big) > 50000 && parse(big, 37, &out));
    assert(out.count == SR_LIST_MAX && strcmp(out.items[12].title, "song12") == 0);
    const char *recent = "{\"items\":[{\"track\":{\"name\":\"最近\\u6b4c\","
        "\"uri\":\"spotify:track:a\",\"artists\":[{\"name\":\"歌手\"}]}},"
        "{\"track\":{\"name\":\"不可播放\",\"is_playable\":false,\"uri\":\"spotify:track:b\"}},"
        "{\"track\":null}],\"next\":null}";
    for (size_t chunk = 1; chunk <= 64; ++chunk) {
        assert(parse_recent(recent, chunk, &out));
        assert(out.count == 2 && out.total == 2);
        assert(strcmp(out.items[0].title, "最近歌") == 0);
        assert(strcmp(out.items[0].subtitle, "歌手") == 0);
        assert(out.items[0].actionable && !out.items[1].actionable);
    }
    assert(parse_recent("{\"items\":[]}", 1, &out) && out.count == 0);
    assert(!parse_recent("{\"items\":[{\"track\":", 2, &out));
    assert(!parse_recent("{\"queue\":[]}", 2, &out));
    n = (size_t)snprintf(big, sizeof(big), "{\"items\":[");
    for (int i = 0; i < 80; ++i) {
        n += (size_t)snprintf(big + n, sizeof(big) - n,
            "%s{\"track\":{\"name\":\"recent%d\",\"uri\":\"spotify:track:a\","
            "\"album\":{\"unused\":\"%0500d\"},\"artists\":[]}}", i ? "," : "", i, i);
    }
    snprintf(big + n, sizeof(big) - n, "]}");
    assert(strlen(big) > 40000 && parse_recent(big, 23, &out));
    assert(out.count == SR_LIST_MAX && out.items[12].actionable);
    puts("queue streaming PASS");
    return 0;
}
