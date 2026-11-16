// main/main.c
// 声环 SoundRing：Spotify 遥控器。初始化板级外设后交给 sr_app。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "sr_app.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_ev_t;

static QueueHandle_t s_keys;
static volatile bool s_input_ready;

// 上电/复位时若某颗键仍被按住，按键组件会把它判成长按。守卫先吞掉动作，
// 直到该键松手（状态机在 key_guard.h，可主机测试）。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 按键回调运行在共享 esp_timer 任务：只入队，不做重活。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready || !s_keys) return;
    const key_ev_t event = {.btn = btn, .ev = ev};
    (void)xQueueSend(s_keys, &event, 0);
}

static void input_task(void *arg) {
    (void)arg;
    key_ev_t event;
    for (;;) {
        if (xQueueReceive(s_keys, &event, portMAX_DELAY) == pdTRUE) {
            sr_app_key(event.btn, event.ev);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "声环 SoundRing 启动");

    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒：先屏蔽动作，直到该键松手");
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，应用无法继续");
        return;
    }
    bsp_display_backlight(80);
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用，界面不显示电量");
    }

    s_keys = xQueueCreate(8, sizeof(key_ev_t));
    if (!s_keys) {
        ESP_LOGE(TAG, "按键队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "sr_keys", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败：界面可显示但无法操作");
    } else if (s_key_guard.armed) {
        // 按键驱动启动前就松手时，组件不会产生 release 事件；按实际电压同步一次。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;

    if (sr_app_start() != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败");
    }
}
