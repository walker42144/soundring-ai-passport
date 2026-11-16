// tests/test_sr_policy.c
// 主机测试：指令门控、轮询退避、Retry-After、429 分类、分页边界与错误文案。
#include "sr_policy.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void check(bool condition, const char *what) {
    if (!condition) {
        ++s_failures;
        printf("FAIL: %s\n", what);
    }
}

static void test_gate(void) {
    sr_gate_t gate;
    sr_gate_init(&gate, 800000);
    check(sr_gate_acquire(&gate, 1000), "首次获取门控");
    check(!sr_gate_acquire(&gate, 2000), "在途时拒绝第二次");
    sr_gate_release(&gate, 3000);
    check(!sr_gate_acquire(&gate, 3000 + 799999), "稳定期内拒绝");
    check(sr_gate_acquire(&gate, 3000 + 800000), "稳定期后允许");
    // 释放后立即重新获取应被 800ms 稳定期挡住（连点场景）。
    sr_gate_release(&gate, 3000 + 800000);
    check(!sr_gate_acquire(&gate, 3000 + 800001), "释放后立刻再获取被拒");
    check(sr_gate_acquire(&gate, 3000 + 1600000), "稳定期结束后再次允许");
}

static void test_retry_after(void) {
    check(sr_retry_after_ms("7", 0) == 7000, "Retry-After 秒");
    check(sr_retry_after_ms("1", 0) == 1000, "Retry-After 最小值 1s");
    check(sr_retry_after_ms("0", 0) == 30000, "零秒按默认 30s");
    check(sr_retry_after_ms("-5", 0) == 30000, "负数按默认");
    check(sr_retry_after_ms("86400", 0) == 86400000, "正好 24h 保持");
    check(sr_retry_after_ms("86401", 0) == 86400000, "超过 24h 收敛");
    check(sr_retry_after_ms("100000", 0) == 86400000, "超长秒数收敛到 24h");
    check(sr_retry_after_ms(NULL, 0) == 30000, "缺失头按默认");
    check(sr_retry_after_ms("", 0) == 30000, "空头按默认");
    check(sr_retry_after_ms("not-a-date", 0) == 30000, "非法值按默认");
    check(sr_retry_after_ms("7s", 0) == 30000, "带单位不算纯秒数");

    // HTTP-date：与 now 比较后落在 1s..24h。
    const int64_t in_window = sr_retry_after_ms("Wed, 21 Oct 2015 07:28:00 GMT", 1445000000000);
    check(in_window >= 1000 && in_window <= 86400000, "HTTP-date 在合理范围");
    const int64_t exact = sr_retry_after_ms("Thu, 01 Jan 1970 00:00:10 GMT", 0);
    check(exact == 10000, "HTTP-date 解析为绝对时间（10s）");
    const int64_t past = sr_retry_after_ms("Thu, 01 Jan 1970 00:00:10 GMT", 2000000000000);
    check(past == 1000, "已过期的 HTTP-date 收敛到 1s");
    const int64_t far = sr_retry_after_ms("Thu, 01 Jan 1970 00:00:10 GMT", -86400000LL * 30);
    check(far == 86400000, "过远的 HTTP-date 收敛到 24h");
}

static void test_backoff(void) {
    check(sr_failure_backoff_ms(0) == 0, "无失败无退避");
    check(sr_failure_backoff_ms(-1) == 0, "负失败数无退避");
    check(sr_failure_backoff_ms(1) == 2000, "第 1 次 2s");
    check(sr_failure_backoff_ms(2) == 4000, "第 2 次 4s");
    check(sr_failure_backoff_ms(3) == 8000, "第 3 次 8s");
    check(sr_failure_backoff_ms(4) == 16000, "第 4 次 16s");
    check(sr_failure_backoff_ms(5) == 32000, "第 5 次 32s");
    check(sr_failure_backoff_ms(9) == 32000, "上限 32s");
    check(sr_failure_backoff_ms(64) == 32000, "极大失败数仍为 32s（不溢出）");
    // 序列必须单调不降。
    for (int i = 1; i < 12; ++i) {
        check(sr_failure_backoff_ms(i + 1) >= sr_failure_backoff_ms(i), "退避序列单调不降");
    }
}

static void test_poll_delay(void) {
    const int64_t start = 1000000;
    // 标准模式：C3 每次请求都是完整 TLS 握手，播放 3s / 暂停 6s。
    check(sr_poll_delay_ms(start, start, 0, true, 0, true) == 3000, "标准播放 3s");
    check(sr_poll_delay_ms(start, start, 0, false, 0, true) == 6000, "标准暂停 6s");
    // 省电模式保持 5s / 15s。
    check(sr_poll_delay_ms(start, start, 0, true, 0, false) == 5000, "省电播放 5s");
    check(sr_poll_delay_ms(start, start, 0, false, 0, false) == 15000, "省电暂停 15s");
    check(sr_poll_delay_ms(start, start, 0, true, 0, SR_POLL_QUICK) == 2000, "快速播放 2s");
    check(sr_poll_delay_ms(start, start, 0, false, 0, SR_POLL_QUICK) == 3000, "快速等待 3s");
    check(sr_poll_delay_ms(start, start, start + 9000000, true, 0, SR_POLL_QUICK) == 9000, "快速模式遵循限流");
    check(sr_poll_delay_ms(start, start, 0, true, 4, SR_POLL_QUICK) == 16000, "快速模式遵循失败退避");
    check(sr_poll_delay_ms(start, start, 0, true, 2, true) == 4000, "失败退避覆盖间隔");
    check(sr_poll_delay_ms(start, start + 1000000, 0, true, 0, true) == 2000, "扣除已耗时");
    check(sr_poll_delay_ms(start, start, start + 9000000, true, 0, true) == 9000, "429 阻塞优先");
    check(sr_poll_delay_ms(start, start + 4000000, 0, true, 0, true) == 100, "最小时延 100ms");
    // now < start（时钟回退/起点在未来）时按“已耗时 0”处理，给满一个间隔。
    check(sr_poll_delay_ms(start, start - 5000, start, true, 0, true) == 3000, "负数耗时按 0 处理");
    // 任何输入组合下都不允许返回负延迟（否则调用处 pdMS_TO_TICKS 溢出）。
    for (int playing = 0; playing <= 1; ++playing) {
        for (int fast = 0; fast <= 2; ++fast) {
            for (int failures = 0; failures < 8; ++failures) {
                const int64_t delay = sr_poll_delay_ms(start, start + 60000000LL, 0,
                                                       playing != 0, failures, (sr_poll_mode_t)fast);
                check(delay >= 100 && delay <= 86400000, "轮询延迟落在 100ms..24h");
            }
        }
    }
}

static void test_429(void) {
    const char *quota_body =
        "{\"error\":{\"status\":429,\"message\":\"quota\",\"reason\":\"QUOTA_EXCEEDED\"}}";
    const char *rate_body = "{\"error\":{\"status\":429,\"message\":\"rate\",\"reason\":\"RATE_LIMITED\"}}";

    check(sr_429_quota(quota_body), "完整响应体识别 QUOTA_EXCEEDED");
    check(sr_429_quota("QUOTA_EXCEEDED"), "只传 reason 也识别");
    check(!sr_429_quota(rate_body), "临时限流不算配额");
    check(!sr_429_quota(""), "空串不算配额");
    check(!sr_429_quota(NULL), "空指针不算配额");

    // 配额用尽：固定 15 分钟，与 Retry-After 无关。
    check(sr_429_block_ms(quota_body, 0) == SR_429_QUOTA_MS, "配额 0 -> 15 分钟");
    check(sr_429_block_ms(quota_body, 5000) == 900000, "配额忽略 Retry-After");
    check(sr_429_block_ms("QUOTA_EXCEEDED", 86400000) == 900000, "配额封顶 15 分钟");

    // 临时限流：Retry-After 钳制到 1s..24h，缺省 30s。
    check(sr_429_block_ms(NULL, 5000) == 5000, "限流按 Retry-After 5s");
    check(sr_429_block_ms(rate_body, 2000) == 2000, "限流按 Retry-After 2s");
    check(sr_429_block_ms(NULL, 0) == 30000, "无限流头按 30s");
    check(sr_429_block_ms(NULL, -1) == 30000, "负 Retry-After 按 30s");
    check(sr_429_block_ms(NULL, 10) == SR_429_MIN_MS, "低于 1s 抬到 1s");
    check(sr_429_block_ms(NULL, 999) == 1000, "999ms 抬到 1s");
    check(sr_429_block_ms(NULL, 1000) == 1000, "1s 保持");
    check(sr_429_block_ms(NULL, 86400000) == 86400000, "24h 保持");
    check(sr_429_block_ms(NULL, 86400001) == 86400000, "超过 24h 收敛");

    // 文案必须区分两类，且都提到下一步。
    check(strstr(sr_429_text(true), "配额") != NULL, "配额文案提到配额");
    check(strstr(sr_429_text(true), "15 分钟") != NULL, "配额文案给出等待时间");
    check(strstr(sr_429_text(false), "限流") != NULL, "限流文案提到限流");
    check(strcmp(sr_429_text(true), sr_429_text(false)) != 0, "两类文案不同");
}

static void test_retry_read(void) {
    check(sr_may_retry_read(401), "401 可重试");
    check(sr_may_retry_read(408), "408 可重试");
    check(sr_may_retry_read(503), "503 可重试");
    check(!sr_may_retry_read(403), "403 不重试");
    check(!sr_may_retry_read(429), "429 不立即重试");
    check(!sr_may_retry_read(200), "200 不需要重试");
}

static void test_paging(void) {
    check(sr_page_clamp(-5, 6, 40) == 0, "负偏移归零");
    check(sr_page_clamp(0, 6, 40) == 0, "首页保持");
    check(sr_page_clamp(6, 6, 40) == 6, "第二页保持");
    check(sr_page_clamp(36, 6, 40) == 36, "合法末页保持");
    check(sr_page_clamp(39, 6, 40) == 39, "末页内的合法偏移保持");
    check(sr_page_clamp(40, 6, 40) == 36, "恰好等于总数回到最后整页");
    check(sr_page_clamp(42, 6, 40) == 36, "越界回到最后整页");
    check(sr_page_clamp(5, 6, 0) == 0, "未知总数归零");
    check(sr_page_clamp(5, 6, -3) == 0, "负总数归零");
    check(sr_page_clamp(5, 0, 10) == 0, "非法 limit 归零");
    check(sr_page_clamp(5, -6, 10) == 0, "负 limit 归零");
    check(sr_page_clamp(0, 6, 1) == 0, "只有 1 项时首页");
    check(sr_page_clamp(7, 1, 3) == 2, "limit=1 时最后一页是 total-1");
}

static void test_errors(void) {
    check(strstr(sr_forbidden_text("PREMIUM_REQUIRED"), "Premium") != NULL, "Premium 文案");
    check(strstr(sr_forbidden_text("NO_ACTIVE_DEVICE"), "设备") != NULL, "无设备文案");
    check(strstr(sr_forbidden_text("INSUFFICIENT_SCOPE"), "权限") != NULL, "权限文案");
    check(strstr(sr_forbidden_text(NULL), "403") != NULL, "未知 403 兜底文案");
    check(strstr(sr_error_text(429, NULL, SR_ERR_READ), "限流") != NULL, "429 文案");
    check(strstr(sr_error_text(500, NULL, SR_ERR_CONTROL), "未确认") != NULL, "控制 5xx 文案");
    check(strstr(sr_error_text(500, NULL, SR_ERR_READ), "异常") != NULL, "读取 5xx 文案");
    check(strstr(sr_error_text(0, NULL, SR_ERR_READ), "Wi-Fi") != NULL, "网络失败文案");
    check(strstr(sr_error_text(401, NULL, SR_ERR_TOKEN), "配对") != NULL, "令牌失败文案");
    check(strstr(sr_error_text(401, NULL, SR_ERR_READ), "配对") != NULL, "读取 401 文案");
}

int main(void) {
    test_gate();
    test_retry_after();
    test_backoff();
    test_poll_delay();
    test_429();
    test_retry_read();
    test_paging();
    test_errors();
    if (s_failures) {
        printf("test_sr_policy: %d 项失败\n", s_failures);
        return 1;
    }
    printf("test_sr_policy: PASS\n");
    return 0;
}
