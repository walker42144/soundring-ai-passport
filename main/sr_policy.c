#include "sr_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 由年月日算 UTC 秒数（Howard Hinnant 的 days_from_civil）。
// 设备端 newlib 与 MSVC 都没有可移植的 timegm，故自带实现。
static int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153U * (month > 2 ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// RFC 1123 解析：只用于 Retry-After，可容忍时区名不匹配。
static bool parse_http_date(const char *value, int64_t *out_ms) {
    if (!value || !*value) return false;
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char weekday[8] = {0};
    char month[8] = {0};
    char tz[8] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(value, "%7[^,], %d %7s %d %d:%d:%d %7s",
               weekday, &day, month, &year, &hour, &minute, &second, tz) < 7) {
        return false;
    }
    int month_index = -1;
    for (int i = 0; i < 12; ++i) {
        if (strcmp(month, months[i]) == 0) {
            month_index = i;
            break;
        }
    }
    if (month_index < 0 || year < 1970 || day < 1 || day > 31) return false;
    const int64_t days = days_from_civil(year, (unsigned)month_index + 1U, (unsigned)day);
    const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    if (seconds <= 0) return false;
    *out_ms = seconds * 1000;
    return true;
}

void sr_gate_init(sr_gate_t *gate, int64_t interval_us) {
    if (!gate) return;
    gate->active = false;
    gate->next_allowed_us = 0;
    gate->interval_us = interval_us;
}

bool sr_gate_acquire(sr_gate_t *gate, int64_t now_us) {
    if (!gate) return false;
    if (gate->active || now_us < gate->next_allowed_us) return false;
    gate->active = true;
    return true;
}

void sr_gate_release(sr_gate_t *gate, int64_t now_us) {
    if (!gate) return;
    gate->active = false;
    gate->next_allowed_us = now_us + gate->interval_us;
}

int64_t sr_retry_after_ms(const char *value, int64_t now_epoch_ms) {
    if (value && *value) {
        char *end = NULL;
        const long seconds = strtol(value, &end, 10);
        if (end && *end == '\0' && seconds > 0) {
            if (seconds > 86400) return 86400 * 1000;
            return (int64_t)seconds * 1000;
        }
        int64_t when_ms = 0;
        if (parse_http_date(value, &when_ms)) {
            int64_t delta = when_ms - now_epoch_ms;
            if (delta < 1000) delta = 1000;
            if (delta > 86400000) delta = 86400000;
            return delta;
        }
    }
    return 30000;
}

int64_t sr_failure_backoff_ms(int failures) {
    if (failures <= 0) return 0;
    const int shift = failures - 1;
    int64_t delay = 2000;
    for (int i = 0; i < shift && i < 4; ++i) delay <<= 1;
    if (delay > 32000) delay = 32000;
    return delay;
}

int64_t sr_poll_delay_ms(int64_t start_us, int64_t now_us, int64_t blocked_until_us,
                         bool playing, int failures, sr_poll_mode_t mode) {
    int64_t interval;
    if (failures > 0) {
        interval = sr_failure_backoff_ms(failures);
    } else if (mode == SR_POLL_QUICK) {
        interval = playing ? 2000 : 3000;
    } else if (mode == SR_POLL_NORMAL) {
        // C3 无 PSRAM、每次请求都是一次完整 TLS 握手，3s/6s 是流畅度与耗电的折中。
        interval = playing ? 3000 : 6000;
    } else {
        interval = playing ? 5000 : 15000;
    }
    const int64_t elapsed_ms = (now_us - start_us) / 1000;
    int64_t delay = interval - (elapsed_ms > 0 ? elapsed_ms : 0);
    const int64_t blocked_ms = (blocked_until_us - now_us) / 1000;
    if (blocked_ms > delay) delay = blocked_ms;
    if (delay < 100) delay = 100;
    return delay;
}

bool sr_429_quota(const char *body_reason_or_body) {
    // 参数既可能是完整响应体，也可能只有 reason，直接子串匹配两种情况都覆盖。
    return body_reason_or_body && strstr(body_reason_or_body, "QUOTA_EXCEEDED") != NULL;
}

int64_t sr_429_block_ms(const char *body_reason_or_body, int64_t retry_after_ms) {
    if (sr_429_quota(body_reason_or_body)) return SR_429_QUOTA_MS;
    if (retry_after_ms <= 0) return SR_429_DEFAULT_MS;
    if (retry_after_ms < SR_429_MIN_MS) return SR_429_MIN_MS;
    if (retry_after_ms > SR_429_MAX_MS) return SR_429_MAX_MS;
    return retry_after_ms;
}

const char *sr_429_text(bool quota) {
    return quota ? "开发模式配额已用尽，15 分钟后重试" : "Spotify 限流，稍后自动重试";
}

bool sr_may_retry_read(int status) {
    switch (status) {
        case 401:
        case 408:
        case 500:
        case 502:
        case 503:
        case 504:
            return true;
        default:
            return false;
    }
}

int sr_page_clamp(int offset, int limit, int total) {
    if (limit <= 0) return 0;
    if (offset < 0) return 0;
    if (total <= 0) return 0;
    if (offset >= total) {
        const int last_page = ((total - 1) / limit) * limit;
        return last_page;
    }
    return offset;
}

const char *sr_forbidden_text(const char *reason) {
    if (!reason) reason = "";
    if (strcmp(reason, "PREMIUM_REQUIRED") == 0) return "此操作需要 Premium";
    if (strcmp(reason, "RESTRICTION_VIOLATED") == 0) return "播放设备暂不允许此操作";
    if (strcmp(reason, "NO_ACTIVE_DEVICE") == 0) return "没有活跃设备，请先打开 Spotify";
    if (strcmp(reason, "INSUFFICIENT_SCOPE") == 0) return "缺少权限，请重新授权";
    if (strcmp(reason, "UNKNOWN") == 0) return "设备或接口拒绝此操作";
    return "设备或接口拒绝此操作（403）";
}

const char *sr_error_text(int status, const char *reason, sr_err_kind_t kind) {
    if (status == 0) return "网络不可用，请检查 Wi-Fi";
    if (status < 0) return "连接失败，请稍后重试";
    if (status == 204) return "操作已发送";
    if (status == 401) {
        return kind == SR_ERR_TOKEN ? "授权刷新失败，请重新配对" : "授权已失效，请重新配对";
    }
    if (status == 403) return sr_forbidden_text(reason);
    if (status == 404) return kind == SR_ERR_READ ? "内容不可用或无权限" : "设备或内容不可用";
    if (status == 429) return "Spotify 限流，稍后自动重试";
    if (status >= 500) {
        return kind == SR_ERR_READ ? "Spotify 服务暂时异常，稍后重试"
                                   : "操作结果未确认，请刷新核对";
    }
    if (status >= 400) return "请求被拒绝，请刷新重试";
    return "已完成";
}
