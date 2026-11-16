#include "sr_progress.h"

#include <math.h>

static double clamp_double(double value, double low, double high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

void sr_progress_init(sr_progress_t *progress) {
    if (!progress) return;
    progress->initialized = false;
    progress->identity = 0;
    progress->anchor_ms = 0;
    progress->anchor_at_us = 0;
    progress->playing = false;
    progress->correction_ms = 0;
    progress->correction_at_us = 0;
    progress->correction_length_ms = 1200;
    progress->duration_ms = 0;
}

int32_t sr_progress_position(const sr_progress_t *progress, int64_t now_us) {
    if (!progress || !progress->initialized) return 0;
    const double elapsed_ms = (now_us - progress->anchor_at_us) > 0
                                  ? (double)(now_us - progress->anchor_at_us) / 1000.0
                                  : 0.0;
    const double t = clamp_double(
        (double)(now_us - progress->correction_at_us) / (progress->correction_length_ms * 1000.0),
        0.0, 1.0);
    const double eased = t * t * (3.0 - 2.0 * t);
    double value = progress->anchor_ms + (progress->playing ? elapsed_ms : 0.0);
    value += progress->correction_ms * (1.0 - eased);
    value = clamp_double(value, 0.0, progress->duration_ms);
    return (int32_t)(value + 0.5);
}

void sr_progress_update(sr_progress_t *progress, uint32_t identity, int32_t position_ms,
                        int32_t duration_ms, bool playing, int64_t now_us) {
    if (!progress) return;
    const double target_duration = duration_ms > 0 ? (double)duration_ms : 0.0;
    const double target = clamp_double((double)position_ms, 0.0, target_duration);
    const double before = (double)sr_progress_position(progress, now_us);
    const bool reset = !progress->initialized || progress->identity != identity ||
                       progress->playing != playing || fabs(before - target) > 3000.0 ||
                       fabs(progress->duration_ms - target_duration) > 1000.0;
    progress->correction_ms = reset ? 0.0 : before - target;
    // smoothstep 最大斜率为 1.5；校正时长 >= 2 倍偏差可保证播放时进度不回退。
    double length = fabs(progress->correction_ms) * 2.0;
    if (length < 1200.0) length = 1200.0;
    progress->correction_length_ms = length;
    progress->correction_at_us = now_us;
    progress->anchor_at_us = now_us;
    progress->anchor_ms = target;
    progress->duration_ms = target_duration;
    progress->playing = playing;
    progress->identity = identity;
    progress->initialized = true;
}

bool sr_progress_needs_frames(const sr_progress_t *progress, int64_t now_us) {
    if (!progress || !progress->initialized || progress->duration_ms <= 0) return false;
    const bool running = progress->playing &&
                         (double)sr_progress_position(progress, now_us) < progress->duration_ms;
    const bool correcting = fabs(progress->correction_ms) > 0.1 &&
                            (double)(now_us - progress->correction_at_us) <
                                progress->correction_length_ms * 1000.0;
    return running || correcting;
}
