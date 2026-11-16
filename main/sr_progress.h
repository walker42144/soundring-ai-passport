// main/sr_progress.h
// 本地单调推进播放进度，并对服务器采样做限速平滑校正（移植手表版 SmoothProgress）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool initialized;
    uint32_t identity;
    double anchor_ms;
    int64_t anchor_at_us;
    bool playing;
    double correction_ms;
    int64_t correction_at_us;
    double correction_length_ms;
    double duration_ms;
} sr_progress_t;

void sr_progress_init(sr_progress_t *progress);
// identity 用曲目 uri 的哈希；换曲、暂停/播放切换或偏差 >3s 时直接重置。
void sr_progress_update(sr_progress_t *progress, uint32_t identity, int32_t position_ms,
                        int32_t duration_ms, bool playing, int64_t now_us);
int32_t sr_progress_position(const sr_progress_t *progress, int64_t now_us);
bool sr_progress_needs_frames(const sr_progress_t *progress, int64_t now_us);
