// main/sr_pair.h
// USB 串口配对：8 位配对码 + PBKDF2 派生密钥 + AES-256-GCM 传输 Wi-Fi 与令牌。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool (*on_wifi)(const char *ssid, const char *pass, void *ctx);
    bool (*on_auth)(const char *client_id, const char *access, const char *refresh,
                    const char *scope, int64_t expires_at_ms, void *ctx);
    void (*on_state)(const char *text, void *ctx);
    // 供 status 命令回报“授权是否已写入”，配置工具据此判断最后一步是否真的成功。
    bool (*query_auth)(void *ctx);
    void *ctx;
} sr_pair_callbacks_t;

esp_err_t sr_pair_start(const sr_pair_callbacks_t *callbacks);
// 生成新配对码并广播 SRPAIR1 起始帧；10 分钟或 5 次失败后失效。
void sr_pair_begin(void);
void sr_pair_end(void);
bool sr_pair_active(void);
const char *sr_pair_code(void);
int sr_pair_seconds_left(void);
