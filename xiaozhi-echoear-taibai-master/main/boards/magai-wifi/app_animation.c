/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"

#include "app_animation.h"
#include "app_weather.h"
#include "weather_display_new.h"
#include "lvgl.h"

static const char *TAG = "app_animation";

// 天气代码在 app_weather.c 中定义，这里通过头文件引用

void updata_weather_icon(void)
{
    // 使用weather_display_new.c中的函数来更新天气图标
    // 这样可以确保使用统一的图标对象和逻辑
    extern void weather_icon_new_update(const char* weather_code);
    
    if (strlen(weather_icon) > 0) {
        weather_icon_new_update(weather_icon);
        ESP_LOGI(TAG, "Updated weather icon for code %s using unified function", weather_icon);
    } else {
        ESP_LOGW(TAG, "Weather icon code is empty, using default");
        weather_icon_new_update("100");
    }
}

/**
 * @brief 初始化天气图标显示
 * 
 * 初始化天气图标显示系统，使用SPIFFS文件系统加载PNG格式的天气图标
 * 
 * @return esp_err_t 初始化结果
 */
esp_err_t app_animation_start(void)
{
    ESP_LOGI(TAG, "Weather animation system initialized with SPIFFS PNG support");
    return ESP_OK;
}