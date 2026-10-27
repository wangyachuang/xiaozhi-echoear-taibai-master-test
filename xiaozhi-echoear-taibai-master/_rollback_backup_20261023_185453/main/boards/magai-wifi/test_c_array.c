/**
 * @file test_c_array.c
 * @brief C数组格式天气图标测试程序
 * 
 * 用于测试C数组格式的天气图标显示功能
 */

#include "weather_display_new.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_c_array";

/**
 * @brief C数组测试任务
 */
void test_c_array_task(void *pvParameters)
{
    ESP_LOGI(TAG, "开始C数组格式天气图标测试");
    
    // 等待系统初始化完成
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // 调用C数组测试函数
    weather_icon_test_c_array();
    
    ESP_LOGI(TAG, "C数组格式天气图标测试完成");
    
    // 删除任务
    vTaskDelete(NULL);
}

/**
 * @brief 启动C数组测试
 */
void start_c_array_test(void)
{
    // 创建测试任务
    xTaskCreate(
        test_c_array_task,
        "test_c_array",
        4096,
        NULL,
        5,
        NULL
    );
}