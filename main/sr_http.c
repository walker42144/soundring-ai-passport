#include "sr_http.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sr_policy.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *TAG = "sr_http";

static void list_heap(const char *stage) {
    ESP_LOGI(TAG, "list %s heap=%u largest=%u min=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

void sr_http_resp_clear(sr_http_resp_t *resp) {
    if (!resp)
        return;
    free(resp->body);
    resp->body = NULL;
    resp->len = 0;
    resp->status = 0;
    resp->retry_after_ms = 0;
    resp->truncated = false;
}

static const char *method_name(sr_method_t method) {
    switch (method) {
    case SR_M_PUT:
        return "PUT";
    case SR_M_POST:
        return "POST";
    case SR_M_DELETE:
        return "DELETE";
    default:
        return "GET";
    }
}

static esp_err_t on_http_event(esp_http_client_event_t *event) {
    sr_http_resp_t *resp = event->user_data;
    if (resp && event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        event->header_value && strcasecmp(event->header_key, "Retry-After") == 0) {
        resp->retry_after_ms = sr_retry_after_ms(event->header_value, (int64_t)time(NULL) * 1000);
    }
    return ESP_OK;
}

static esp_err_t http_do(sr_method_t method, const char *url, const char *bearer,
                     const char *content_type, const char *body, sr_http_resp_t *resp,
                     size_t max_body, sr_http_chunk_fn consume, void *context) {
    if (!url || !resp)
        return ESP_ERR_INVALID_ARG;
    memset(resp, 0, sizeof(*resp));
    const bool list_diag = strstr(url, "/playlists") || strstr(url, "/player/queue") ||
                           strstr(url, "/recently-played");
    if (list_diag) list_heap("before connect");

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = on_http_event,
        .user_data = resp,
        .method = method == SR_M_PUT      ? HTTP_METHOD_PUT
                  : method == SR_M_POST   ? HTTP_METHOD_POST
                  : method == SR_M_DELETE ? HTTP_METHOD_DELETE
                                          : HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_ERR_NO_MEM;

    if (bearer && *bearer) {
        esp_http_client_set_header(client, "Authorization", bearer);
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    const int write_length = (body && method != SR_M_GET) ? (int)strlen(body) : 0;
    if (write_length > 0) {
        esp_http_client_set_header(client, "Content-Type",
                                   content_type ? content_type : "application/json");
    }

    esp_err_t err = esp_http_client_open(client, write_length);
    if (err != ESP_OK) {
        int tls_error = 0, tls_flags = 0;
        (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
        ESP_LOGW(TAG, "%s 连接失败: %s errno=%d tls=%d flags=%d heap=%u largest=%u",
                 method_name(method), esp_err_to_name(err), esp_http_client_get_errno(client),
                 tls_error, tls_flags, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        esp_http_client_cleanup(client);
        return err;
    }
    if (write_length > 0) {
        if (esp_http_client_write(client, body, write_length) != write_length) {
            ESP_LOGW(TAG, "%s 请求体写入失败", method_name(method));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }
    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGW(TAG, "%s 读取响应头失败", method_name(method));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    resp->status = esp_http_client_get_status_code(client);
    if (list_diag) list_heap("headers received");

    if (resp->status >= 200 && resp->status < 300 && consume) {
        char chunk[512];
        size_t received = 0;
        for (;;) {
            int n = esp_http_client_read(client, chunk, sizeof(chunk));
            if (n < 0 || (n == 0 && content_length > 0 && received < (uint64_t)content_length)) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            if (!n) break;
            received += (size_t)n;
            if (received > 1024U * 1024U || !consume(chunk, (size_t)n, context)) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            if (content_length > 0 && received >= (uint64_t)content_length) break;
        }
        resp->len = received;
    } else if (resp->status >= 200 && resp->status < 300 && max_body > 0) {
        // Known lengths avoid reserving the full limit while TLS holds heap.
        // Unknown/chunked responses grow on demand, always bounded by max_body.
        size_t cap = content_length > 0 && (uint64_t)content_length < max_body
                         ? (size_t)content_length
                         : (max_body < 1024 ? max_body : 1024);
        if (content_length > 0 && (uint64_t)content_length >= max_body)
            cap = max_body;
        char *buffer = malloc(cap + 1);
        if (!buffer) {
            ESP_LOGW(TAG, "%s 响应缓冲分配失败 (%u 字节)", method_name(method),
                     (unsigned)(cap + 1));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
        size_t used = 0;
        while (used < max_body) {
            if (content_length > 0 && (uint64_t)used >= (uint64_t)content_length)
                break;
            if (used == cap) {
                size_t next = cap > max_body / 2 ? max_body : cap * 2;
                char *grown = realloc(buffer, next + 1);
                if (!grown) {
                    ESP_LOGW(TAG, "%s 响应缓冲扩展失败 (%u 字节)", method_name(method),
                             (unsigned)(next + 1));
                    free(buffer);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    return ESP_ERR_NO_MEM;
                }
                buffer = grown;
                cap = next;
            }
            const int read = esp_http_client_read(client, buffer + used, (int)(cap - used));
            if (read < 0) {
                free(buffer);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
            if (read == 0)
                break;
            used += (size_t)read;
        }
        if (content_length > 0 && (uint64_t)used < (uint64_t)content_length && used < max_body) {
            ESP_LOGW(TAG, "%s 响应提前结束", method_name(method));
            free(buffer);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        buffer[used] = '\0';
        resp->body = buffer;
        resp->len = used;
        if (used >= max_body) {
            char extra;
            resp->truncated = content_length > 0 ? (uint64_t)content_length > used
                                                  : esp_http_client_read(client, &extra, 1) > 0;
            if (resp->truncated)
                ESP_LOGW(TAG, "响应超过上限 %u 字节", (unsigned)max_body);
        }
    } else if (resp->status >= 400) {
        // 错误体只保留 1KB，用于读取 Spotify 的 reason 字段。
        // 429 也要读：body 里的 QUOTA_EXCEEDED 决定是配额用尽还是临时限流。
        const size_t cap = 1024;
        char *buffer = malloc(cap + 1);
        if (buffer) {
            size_t used = 0;
            while (used < cap) {
                const int read = esp_http_client_read(client, buffer + used, (int)(cap - used));
                if (read <= 0)
                    break;
                used += (size_t)read;
            }
            buffer[used] = '\0';
            resp->body = buffer;
            resp->len = used;
        }
    } else {
        // 其余响应丢弃响应体，避免无界内存增长。
        char sink[128];
        for (int i = 0; i < 32; ++i) {
            if (esp_http_client_read(client, sink, sizeof(sink)) <= 0)
                break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (list_diag) list_heap("connection closed");
    return ESP_OK;
}

esp_err_t sr_http_do(sr_method_t method, const char *url, const char *bearer,
                     const char *content_type, const char *body, sr_http_resp_t *resp,
                     size_t max_body) {
    return http_do(method, url, bearer, content_type, body, resp, max_body, NULL, NULL);
}

esp_err_t sr_http_stream_get(const char *url, const char *bearer, sr_http_resp_t *resp,
                             sr_http_chunk_fn consume, void *context) {
    if (!consume) return ESP_ERR_INVALID_ARG;
    return http_do(SR_M_GET, url, bearer, NULL, NULL, resp, 0, consume, context);
}
