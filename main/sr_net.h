// main/sr_net.h
// Wi-Fi STA + SNTP。事件回调只置标志/排定时器，不做阻塞动作。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool started;
    bool connected;
    bool time_valid;
    char ip[16];
    char ssid[33];
    int rssi;
} sr_net_status_t;

// 连接状态变化时回调（运行在事件任务，禁止阻塞/LVGL）。
typedef void (*sr_net_event_cb_t)(void);

esp_err_t sr_net_start(const char *ssid, const char *pass);
void sr_net_get_status(sr_net_status_t *status);
bool sr_net_connected(void);
bool sr_net_time_valid(void);
void sr_net_set_event_cb(sr_net_event_cb_t cb);
