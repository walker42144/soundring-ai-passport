// main/sr_policy.h
// 纯逻辑：指令门控、轮询/退避、限流解析、错误文案。不依赖 ESP-IDF，便于主机测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// 一次只允许一条在途指令；放开后留出稳定间隔，避免连点排队。
// 沿用手表版 CommandGate（800ms 间隔），时钟改为单调微秒。
typedef struct {
    bool active;
    int64_t next_allowed_us;
    int64_t interval_us;
} sr_gate_t;

void sr_gate_init(sr_gate_t *gate, int64_t interval_us);
bool sr_gate_acquire(sr_gate_t *gate, int64_t now_us);
void sr_gate_release(sr_gate_t *gate, int64_t now_us);

// Retry-After 支持秒数与 HTTP-date；无法解析时按 30s，范围 1s..24h。
int64_t sr_retry_after_ms(const char *value, int64_t now_epoch_ms);
// 读取失败退避：2s << (failures-1)，上限 32s。
int64_t sr_failure_backoff_ms(int failures);
// Percent of the allocatable heap currently allocated; -1 if total is unavailable.
int sr_heap_used_percent(size_t used, size_t free_bytes);
// Whether a scheduled request can run, including a one-shot wake refresh.
bool sr_poll_ready(bool playback_page, bool sleeping, bool wake_pending, bool pairing,
                   bool connected, bool time_valid, bool authorized, bool blocked,
                   int64_t now_us, int64_t next_us);
// 下一次轮询的等待时间（毫秒）。blocked_until_us 来自 429。
typedef enum { SR_POLL_ECO = 0, SR_POLL_NORMAL = 1, SR_POLL_QUICK = 2,
               SR_POLL_AUTO = 3, SR_POLL_SCREEN_OFF = 4 } sr_poll_mode_t;
// Screen-off always uses the slow schedule, regardless of the selected mode.
sr_poll_mode_t sr_effective_poll_mode(sr_poll_mode_t selected, bool screen_off);
// Zero disables the corresponding timeout; both values are measured from the last key press.
bool sr_power_dim_due(int64_t idle_us, uint16_t dim_sec);
bool sr_power_off_due(int64_t idle_us, uint16_t off_sec);
typedef enum { SR_POWER_IDLE, SR_POWER_DIM, SR_POWER_OFF } sr_power_action_t;
// Always-on suppresses automatic off, but uses the same dim timer.
sr_power_action_t sr_power_idle_action(bool always_on, bool dimmed, int64_t idle_us,
                                      uint16_t dim_sec, uint16_t off_sec);
// The first recognized key gesture restores brightness only.
bool sr_power_key_wakes_only(bool suspended, bool dimmed);
uint16_t sr_power_next_timeout(uint16_t current);
// 快速：播放 2s / 暂停或无播放 3s；标准：3s / 6s；省电：5s / 15s。
// 失败退避和 Spotify 429 封禁在所有模式下始终生效。
int64_t sr_poll_delay_ms(int64_t start_us, int64_t now_us, int64_t blocked_until_us,
                         bool playing, int failures, sr_poll_mode_t mode);

// 429 分两类：开发模式配额用尽（QUOTA_EXCEEDED，等 15 分钟才可能恢复）与临时限流
// （按 Retry-After 退避）。body_reason_or_body 可传完整 JSON 体，也可只传 reason。
#define SR_429_QUOTA_MS 900000
#define SR_429_DEFAULT_MS 30000
#define SR_429_MIN_MS 1000
#define SR_429_MAX_MS 86400000
bool sr_429_quota(const char *body_reason_or_body);
int64_t sr_429_block_ms(const char *body_reason_or_body, int64_t retry_after_ms);
const char *sr_429_text(bool quota);
bool sr_may_retry_read(int status);
int sr_page_clamp(int offset, int limit, int total);
// Spotify playlist path parameters are IDs, not URIs or display text.
bool sr_playlist_id_valid(const char *id);
bool sr_list_request_current(uint32_t request, uint32_t current, int request_screen,
                             int current_screen, int request_offset, int current_offset);

typedef enum {
    SR_ERR_READ = 0,
    SR_ERR_CONTROL,
    SR_ERR_TOKEN,
} sr_err_kind_t;

// 403 原因、5xx、429、401 的用户可读中文说明（手表版 ControlPolicy 的扩展）。
const char *sr_error_text(int status, const char *reason, sr_err_kind_t kind);
const char *sr_forbidden_text(const char *reason);
