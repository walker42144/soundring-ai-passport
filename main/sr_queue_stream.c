#include "sr_queue_stream.h"
#include "sr_text.h"
#include "cJSON.h"
#include <string.h>

enum { ROOT = 1, QUEUE, TRACK, ARTISTS, ARTIST, OTHER, RECENT_ITEMS, RECENT_ENTRY };

void sr_queue_stream_init(sr_queue_stream_t *r, sr_list_t *out) {
    memset(r, 0, sizeof(*r));
    r->out = out;
    memset(out, 0, sizeof(*out));
}

void sr_recent_stream_init(sr_queue_stream_t *r, sr_list_t *out) {
    sr_queue_stream_init(r, out);
    r->recent = true;
}

static bool value_expected(const sr_queue_frame_t *f) {
    return (f->type == '{' && f->state == 2) || (f->type == '[' && (f->state == 0 || f->state == 4));
}

static void value_done(sr_queue_stream_t *r) {
    if (r->depth) r->frames[r->depth - 1].state = 3;
    else r->complete = true;
}

static bool begin_container(sr_queue_stream_t *r, char type) {
    if (r->depth >= (int)(sizeof(r->frames) / sizeof(r->frames[0]))) return false;
    int role = OTHER;
    if (!r->depth) {
        if (r->complete || type != '{') return false;
        role = ROOT;
    } else {
        sr_queue_frame_t *p = &r->frames[r->depth - 1];
        if (!value_expected(p)) return false;
        if (p->role == ROOT && type == '[' &&
            strcmp(p->key, r->recent ? "items" : "queue") == 0) {
            role = r->recent ? RECENT_ITEMS : QUEUE;
            r->queue_seen = true;
        } else if (type == '{' && p->role == RECENT_ITEMS) {
            role = RECENT_ENTRY;
        } else if (type == '{' && p->role == RECENT_ENTRY && strcmp(p->key, "track") == 0) {
            role = TRACK;
            memset(&r->item, 0, sizeof(r->item));
            r->item.position = -1;
            r->explicitly_unplayable = false;
        } else if (type == '{' && ((p->role == ROOT && strcmp(p->key, "currently_playing") == 0) || p->role == QUEUE)) {
            role = TRACK;
            memset(&r->item, 0, sizeof(r->item));
            r->item.position = -1;
        } else if (p->role == TRACK && type == '[' && strcmp(p->key, "artists") == 0) role = ARTISTS;
        else if (p->role == ARTISTS && type == '{') role = ARTIST;
    }
    sr_queue_frame_t *f = &r->frames[r->depth++];
    memset(f, 0, sizeof(*f));
    f->type = type;
    f->role = (char)role;
    return true;
}

static bool end_container(sr_queue_stream_t *r, char type) {
    if (!r->depth) return false;
    sr_queue_frame_t *f = &r->frames[r->depth - 1];
    if (f->type != type || (f->state != 0 && f->state != 3)) return false;
    if (f->role == TRACK && r->item.title[0] && r->out->count < SR_LIST_MAX) {
        r->item.actionable = r->recent && r->item.uri[0] && !r->explicitly_unplayable;
        r->out->items[r->out->count++] = r->item;
    }
    --r->depth;
    value_done(r);
    return true;
}

static void append_artist(sr_item_t *item, const char *name) {
    size_t n = strlen(item->subtitle);
    if (n && n + 3 < sizeof(item->subtitle)) {
        memcpy(item->subtitle + n, " / ", 3);
        n += 3;
        item->subtitle[n] = 0;
    }
    if (n < sizeof(item->subtitle))
        sr_utf8_ellipsis(name, item->subtitle + n, sizeof(item->subtitle) - n);
}

static bool string_done(sr_queue_stream_t *r) {
    if (!r->depth) return false;
    sr_queue_frame_t *f = &r->frames[r->depth - 1];
    bool is_key = f->type == '{' && (f->state == 0 || f->state == 4);
    if (!is_key && !value_expected(f)) return false;
    if (!r->token_overflow) {
        r->token[r->token_len++] = '"';
        r->token[r->token_len] = 0;
        cJSON *s = cJSON_Parse(r->token);
        if (!cJSON_IsString(s) || !s->valuestring) { if (s) cJSON_Delete(s); return false; }
        if (is_key) sr_utf8_ellipsis(s->valuestring, f->key, sizeof(f->key));
        else if (f->role == TRACK && strcmp(f->key, "name") == 0)
            sr_utf8_ellipsis(s->valuestring, r->item.title, sizeof(r->item.title));
        else if (f->role == TRACK && strcmp(f->key, "uri") == 0)
            sr_utf8_ellipsis(s->valuestring, r->item.uri, sizeof(r->item.uri));
        else if (f->role == ARTIST && strcmp(f->key, "name") == 0)
            append_artist(&r->item, s->valuestring);
        cJSON_Delete(s);
    } else if (is_key) f->key[0] = 0;
    if (is_key) f->state = 1;
    else value_done(r);
    return true;
}

static bool consume(sr_queue_stream_t *r, unsigned char c) {
again:
    if (r->in_string) {
        if (!r->escape && c == '"') {
            r->in_string = false;
            return string_done(r);
        }
        if (c < 0x20) return false;
        if (r->token_len + 2 < sizeof(r->token)) r->token[r->token_len++] = (char)c;
        else r->token_overflow = true;
        if (!r->escape && c == '\\') r->escape = true;
        else r->escape = false;
        return true;
    }
    if (r->primitive) {
        if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (r->token_overflow) return false;
            r->token[r->token_len] = 0;
            cJSON *scalar = cJSON_ParseWithOpts(r->token, NULL, 1);
            if (!scalar || !(cJSON_IsNull(scalar) || cJSON_IsBool(scalar) || cJSON_IsNumber(scalar))) {
                if (scalar) cJSON_Delete(scalar);
                return false;
            }
            if (r->depth) {
                sr_queue_frame_t *f = &r->frames[r->depth - 1];
                if (f->role == TRACK && strcmp(f->key, "is_playable") == 0 && cJSON_IsFalse(scalar))
                    r->explicitly_unplayable = true;
            }
            cJSON_Delete(scalar);
            r->primitive = false;
            value_done(r);
            goto again;
        }
        if (r->token_len + 1 < sizeof(r->token)) r->token[r->token_len++] = (char)c;
        else r->token_overflow = true;
        return true;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return true;
    if (c == '{' || c == '[') return begin_container(r, (char)c);
    if (c == '}' || c == ']') return end_container(r, c == '}' ? '{' : '[');
    if (!r->depth || r->complete) return false;
    sr_queue_frame_t *f = &r->frames[r->depth - 1];
    if (c == ':') { if (f->type != '{' || f->state != 1) return false; f->state = 2; return true; }
    if (c == ',') { if (f->state != 3) return false; f->state = 4; f->key[0] = 0; return true; }
    if (c == '"') {
        if (!(f->type == '{' && (f->state == 0 || f->state == 4)) && !value_expected(f)) return false;
        r->in_string = true; r->escape = false; r->token_overflow = false;
        r->token_len = 1; r->token[0] = '"'; return true;
    }
    if (!value_expected(f)) return false;
    r->primitive = true; r->token_len = 1; r->token[0] = (char)c; r->token_overflow = false;
    return true;
}

bool sr_queue_stream_feed(const char *data, size_t len, void *context) {
    sr_queue_stream_t *r = context;
    if (!r || r->failed) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!consume(r, (unsigned char)data[i])) { r->failed = true; return false; }
    }
    return true;
}

bool sr_queue_stream_finish(sr_queue_stream_t *r) {
    if (!r || r->failed || r->in_string || r->primitive || r->depth || !r->complete || !r->queue_seen)
        return false;
    r->out->total = r->out->count;
    return true;
}
