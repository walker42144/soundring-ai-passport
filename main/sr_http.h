// main/sr_http.h
// 有界 HTTPS 请求：固定超时、根证书校验、响应体上限、Retry-After 提取。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SR_M_GET = 0,
    SR_M_PUT,
    SR_M_POST,
    SR_M_DELETE,
} sr_method_t;

typedef struct {
    int status;            // HTTP 状态码；0=传输失败
    int64_t retry_after_ms;
    char *body;            // 成功时最多 max_body 字节；>=400 时最多 1KB（含 429）；调用方 free
    size_t len;
    bool truncated;
} sr_http_resp_t;

typedef bool (*sr_http_chunk_fn)(const char *data, size_t len, void *context);
esp_err_t sr_http_stream_get(const char *url, const char *bearer, sr_http_resp_t *resp,
                             sr_http_chunk_fn consume, void *context);

void sr_http_resp_clear(sr_http_resp_t *resp);

// body 仅对 PUT/POST 使用；bearer 为空则不带 Authorization。
esp_err_t sr_http_do(sr_method_t method, const char *url, const char *bearer,
                     const char *content_type, const char *body,
                     sr_http_resp_t *resp, size_t max_body);
