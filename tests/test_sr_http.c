#include "esp_http_client.h"
#include "sr_http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocation_ceiling = (size_t)-1;
static void *tracked_malloc(size_t n) { return n > allocation_ceiling ? NULL : malloc(n); }
static void *tracked_realloc(void *p, size_t n) {
    return n > allocation_ceiling ? NULL : realloc(p, n);
}
#define malloc tracked_malloc
#define realloc tracked_realloc
#include "../main/sr_http.c"
#undef malloc
#undef realloc
struct fake_client {
    esp_http_client_config_t cfg;
    int status;
    const char *response;
    int pos;
};
static struct fake_client fake;
static int next_status;
static const char *next_response;
static int next_unknown_length;
static int fail_open;
static int64_t header_override = -1;
static bool count_chunk(const char *data, size_t len, void *context) {
    (void)data;
    *(size_t *)context += len;
    return true;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    memset(&fake, 0, sizeof(fake));
    fake.cfg = *cfg;
    fake.status = next_status;
    fake.response = next_response;
    return &fake;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) {
    (void)c;
    (void)k;
    (void)v;
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n) {
    (void)c;
    (void)n;
    return fail_open ? ESP_FAIL : ESP_OK;
}
int esp_http_client_get_errno(esp_http_client_handle_t c) {
    (void)c;
    return 0;
}
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c, int *error,
                                                       int *flags) {
    (void)c;
    *error = 0;
    *flags = 0;
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *p, int n) {
    (void)c;
    (void)p;
    return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    if (c->status == 429) {
        esp_http_client_event_t e = {HTTP_EVENT_ON_HEADER, c->cfg.user_data, "Retry-After", "17"};
        c->cfg.event_handler(&e);
    }
    if (header_override >= 0)
        return header_override;
    return next_unknown_length ? 0 : (int64_t)strlen(c->response);
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
int esp_http_client_read(esp_http_client_handle_t c, char *p, int n) {
    size_t rem = strlen(c->response) - (size_t)c->pos;
    if (rem > (size_t)n)
        rem = (size_t)n;
    memcpy(p, c->response + c->pos, rem);
    c->pos += (int)rem;
    return (int)rem;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    (void)c;
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    (void)c;
    return ESP_OK;
}
int main(void) {
    sr_http_resp_t response;
    next_status = 200;
    next_response = "{\"access_token\":\"new\"}";
    assert(sr_http_do(SR_M_POST, "https://example", NULL, "application/x-www-form-urlencoded",
                      "x=y", &response, 128) == ESP_OK);
    assert(response.status == 200 && response.body && strcmp(response.body, next_response) == 0);
    sr_http_resp_clear(&response);
    next_status = 429;
    next_response = "{\"error\":\"limited\"}";
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 128) == ESP_OK);
    assert(response.status == 429 && response.retry_after_ms == 17000 && response.body);
    sr_http_resp_clear(&response);
    next_status = 204;
    next_response = "";
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 128) == ESP_OK);
    assert(response.status == 204 && response.len == 0);
    sr_http_resp_clear(&response);
    static char small_playlist[3001];
    memset(small_playlist, 'x', sizeof(small_playlist) - 1);
    small_playlist[sizeof(small_playlist) - 1] = '\0';
    next_status = 200;
    next_response = small_playlist;
    allocation_ceiling = 5000; // A 16 KB reservation fails; an actual 3 KB response must work.
    next_unknown_length = 0;
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 16384) == ESP_OK);
    assert(response.len == 3000 && strcmp(response.body, small_playlist) == 0);
    sr_http_resp_clear(&response);
    next_unknown_length = 1; // chunked or missing Content-Length grows as bytes arrive
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 16384) == ESP_OK);
    assert(response.len == 3000 && strcmp(response.body, small_playlist) == 0);
    sr_http_resp_clear(&response);
    next_response = "123456789";
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 8) == ESP_OK);
    assert(response.truncated);
    sr_http_resp_clear(&response);
    next_response = "12345678";
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 8) == ESP_OK);
    assert(!response.truncated);
    sr_http_resp_clear(&response);
    next_response = small_playlist;
    size_t streamed = 0;
    allocation_ceiling = 16; // Streaming must not allocate a response buffer.
    assert(sr_http_stream_get("https://example", NULL, &response, count_chunk, &streamed) == ESP_OK);
    assert(streamed == 3000 && response.body == NULL);
    sr_http_resp_clear(&response);
    allocation_ceiling = 5000;
    next_unknown_length = 0;
    header_override = 4000;
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 16384) == ESP_FAIL);
    assert(response.body == NULL);
    header_override = -1;
    next_unknown_length = 1;
    allocation_ceiling = 1000;
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 16384) ==
           ESP_ERR_NO_MEM);
    assert(response.body == NULL);
    fail_open = 1;
    assert(sr_http_do(SR_M_GET, "https://example", NULL, NULL, NULL, &response, 16384) == ESP_FAIL);
    assert(response.body == NULL);
    puts("HTTP response and Retry-After PASS");
    return 0;
}
