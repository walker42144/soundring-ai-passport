#pragma once
#include "sr_spotify.h"

// A bounded incremental JSON reader for Spotify's full queue response.
typedef struct {
    char type, state, role;
    char key[32];
} sr_queue_frame_t;

typedef struct {
    sr_queue_frame_t frames[20];
    int depth;
    char token[1024];
    size_t token_len;
    bool token_overflow, in_string, escape, primitive, failed, complete, queue_seen, recent;
    bool explicitly_unplayable;
    sr_item_t item;
    sr_list_t *out;
} sr_queue_stream_t;

void sr_queue_stream_init(sr_queue_stream_t *reader, sr_list_t *out);
void sr_recent_stream_init(sr_queue_stream_t *reader, sr_list_t *out);
bool sr_queue_stream_feed(const char *data, size_t len, void *context);
bool sr_queue_stream_finish(sr_queue_stream_t *reader);
