#include "sr_store.h"
#include "sr_policy.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t saved_poll, saved_always;
static bool have_always;
static bool have_mode;
static uint16_t saved_dim, saved_off;
static bool have_dim, have_off;
esp_err_t nvs_open(const char *n, nvs_open_mode_t m, nvs_handle_t *h) { (void)n;(void)m;*h=1;return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *v,size_t *n) { (void)h;(void)k;(void)v;(void)n;return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v) { (void)h;if(!strcmp(k,"always") && have_always){*v=saved_always;return ESP_OK;}if(strcmp(k,"pmode") || !have_mode)return ESP_ERR_NVS_NOT_FOUND;*v=saved_poll;return ESP_OK; }
esp_err_t nvs_get_u16(nvs_handle_t h,const char *k,uint16_t *v) { (void)h;if(!strcmp(k,"dim")&&have_dim){*v=saved_dim;return ESP_OK;}if(!strcmp(k,"off")&&have_off){*v=saved_off;return ESP_OK;}return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_i64(nvs_handle_t h,const char *k,int64_t *v) { (void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *v) { (void)h;(void)k;(void)v;return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v) { (void)h;if(!strcmp(k,"always")){saved_always=v;have_always=true;}if(!strcmp(k,"pmode")){saved_poll=v;have_mode=true;}return ESP_OK; }
esp_err_t nvs_set_u16(nvs_handle_t h,const char *k,uint16_t v) { (void)h;if(!strcmp(k,"dim")){saved_dim=v;have_dim=true;}if(!strcmp(k,"off")){saved_off=v;have_off=true;}return ESP_OK; }
esp_err_t nvs_set_i64(nvs_handle_t h,const char *k,int64_t v) { (void)h;(void)k;(void)v;return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k) { (void)h;(void)k;return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h;return ESP_OK; }
int main(void) {
    sr_config_t c;
    sr_store_load(&c);
    assert(c.poll_mode == SR_POLL_AUTO && c.dim_sec == 60 && c.off_sec == 90);
    have_dim=true; saved_dim=120;
    sr_store_load(&c);
    assert(c.dim_sec == 120 && c.off_sec == 150);
    saved_dim=0;
    sr_store_load(&c);
    assert(c.off_sec == 0);
    have_mode=true;
    for (int i=0;i<=3;++i) {
        saved_poll=(uint8_t)i;
        sr_store_load(&c);
        assert(c.poll_mode == i);
        assert(sr_store_save_settings(&c) == ESP_OK && saved_poll == i);
    }
    saved_poll=255;
    sr_store_load(&c);
    assert(c.poll_mode == SR_POLL_AUTO);
    c.poll_mode=255;
    assert(sr_store_save_settings(&c) == ESP_OK && saved_poll == SR_POLL_AUTO);
    c.dim_sec=15; c.off_sec=120;
    assert(sr_store_save_settings(&c) == ESP_OK);
    sr_store_load(&c);
    assert(c.dim_sec == 15 && c.off_sec == 120);
    c.always_on=1;
    assert(sr_store_save_settings(&c) == ESP_OK);
    sr_store_load(&c);
    assert(c.always_on == 1 && c.dim_sec == 15 && c.off_sec == 120);
    c.always_on=0;
    assert(sr_store_save_settings(&c) == ESP_OK);
    sr_store_load(&c);
    assert(c.always_on == 0 && c.dim_sec == 15 && c.off_sec == 120);
    puts("power settings persistence PASS");
    return 0;
}
