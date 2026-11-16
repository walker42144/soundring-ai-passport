#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct fake_client *esp_http_client_handle_t;
typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_PUT,
    HTTP_METHOD_POST,
    HTTP_METHOD_DELETE
} esp_http_client_method_t;
typedef enum { HTTP_EVENT_ON_HEADER } esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    esp_http_client_method_t method;
    int timeout_ms, buffer_size, buffer_size_tx;
    bool disable_auto_redirect, keep_alive_enable;
    int (*crt_bundle_attach)(void *);
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key,
                                     const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int size);
int esp_http_client_write(esp_http_client_handle_t client, const char *body, int size);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *data, int size);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

int esp_http_client_get_errno(esp_http_client_handle_t client);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t client, int *error,
                                                       int *flags);
