/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 更新天气图标
 * 
 * 根据天气代码更新UI上的天气图标
 */
void updata_weather_icon(void);

/**
 * @brief 初始化天气图标显示
 * 
 * 初始化天气图标显示系统，使用SPIFFS文件系统加载PNG格式的天气图标
 * 
 * @return esp_err_t 初始化结果
 */
esp_err_t app_animation_start(void);

#ifdef __cplusplus
}
#endif