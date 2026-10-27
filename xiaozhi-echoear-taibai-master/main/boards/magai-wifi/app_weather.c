/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "app_weather.h"
#include "esp_log.h"

static const char *TAG = "app_weather";

/**
 * @brief 天气代码
 * 
 * 存储当前天气代码，用于查找对应的天气图标
 * 默认为晴天代码 "100"
 */
char weather_icon[10] = "100";