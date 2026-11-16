// tests/test_sr_progress.c
// 主机测试：本地进度推进、平滑校正、切歌重置与边界收敛。
#include "sr_progress.h"

#include <stdio.h>

static int s_failures;

static void check(bool condition, const char *what) {
    if (!condition) {
        ++s_failures;
        printf("FAIL: %s\n", what);
    }
}

static bool near(int32_t value, int32_t expected, int32_t tolerance) {
    const int32_t delta = value - expected;
    return delta <= tolerance && delta >= -tolerance;
}

int main(void) {
    sr_progress_t progress;
    sr_progress_init(&progress);
    check(sr_progress_position(&progress, 0) == 0, "未初始化时为 0");

    const int64_t t0 = 1000000;
    sr_progress_update(&progress, 42, 10000, 200000, true, t0);
    check(near(sr_progress_position(&progress, t0), 10000, 1), "初始锚点");
    check(near(sr_progress_position(&progress, t0 + 5000000), 15000, 50), "播放推进 5 秒");

    sr_progress_update(&progress, 42, 16000, 200000, false, t0 + 6000000);
    const int32_t paused = sr_progress_position(&progress, t0 + 6000000);
    check(near(sr_progress_position(&progress, t0 + 20000000), paused, 1), "暂停后不再推进");

    // 微小偏差：平滑校正，且播放方向不回退。
    sr_progress_update(&progress, 42, 21000, 200000, false, t0 + 20000000);
    sr_progress_update(&progress, 42, 21000, 200000, true, t0 + 20000000);
    sr_progress_update(&progress, 42, 20500, 200000, true, t0 + 20000000);
    int32_t previous = sr_progress_position(&progress, t0 + 20000000);
    bool monotonic = true;
    for (int i = 1; i <= 40; ++i) {
        const int32_t now = sr_progress_position(&progress, t0 + 20000000 + i * 50000);
        if (now + 1 < previous) monotonic = false;
        previous = now;
    }
    check(monotonic, "校正期间进度不回退");
    check(near(sr_progress_position(&progress, t0 + 20000000 + 2000000), 22500, 800),
          "校正后贴近服务器");

    // 大偏差：直接重置。
    sr_progress_update(&progress, 42, 150000, 200000, true, t0 + 30000000);
    check(near(sr_progress_position(&progress, t0 + 30000000), 150000, 1), "大偏差立即重置");

    // 切歌：identity 变化立即重置。
    sr_progress_update(&progress, 43, 5000, 180000, true, t0 + 31000000);
    check(near(sr_progress_position(&progress, t0 + 31000000), 5000, 1), "切歌重置");
    check(near(sr_progress_position(&progress, t0 + 31000000 + 200000000), 180000, 1),
          "进度收敛到总长");

    check(sr_progress_needs_frames(&progress, t0 + 31000000), "播放时需要刷新帧");
    sr_progress_update(&progress, 43, 180000, 180000, false, t0 + 32000000);
    check(!sr_progress_needs_frames(&progress, t0 + 33000000), "结束后无需刷新帧");

    if (s_failures) {
        printf("test_sr_progress: %d 项失败\n", s_failures);
        return 1;
    }
    printf("test_sr_progress: PASS\n");
    return 0;
}
