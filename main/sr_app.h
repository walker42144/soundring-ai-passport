// main/sr_app.h
// 声环 SoundRing：Spotify 遥控器应用入口。按键由 main.c 的输入任务转发。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

esp_err_t sr_app_start(void);
void sr_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
