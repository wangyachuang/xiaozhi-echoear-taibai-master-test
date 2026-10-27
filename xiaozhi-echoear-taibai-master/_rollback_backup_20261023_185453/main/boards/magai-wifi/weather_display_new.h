/**
 * @file weather_display_new.h
 * @brief 天气图标显示模块头文件 - 新版本
 * 
 * 本模块提供天气图标显示相关的功能，包括：
 * 1. 初始化天气图标显示
 * 2. 更新天气图标
 * 3. 显示指定类型的天气图标
 * 4. 获取天气图标对象
 * 
 * 本模块直接加载PNG格式的天气图标，通过SPIFFS文件系统访问。
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化天气图标显示模块
 * 
 * 初始化天气图标显示相关的资源。
 * 必须在使用其他天气图标功能之前调用此函数。
 * 
 * @param parent 父对象，天气图标将作为此对象的子对象创建
 * @return esp_err_t 初始化结果
 *         - ESP_OK: 初始化成功
 *         - ESP_FAIL: 初始化失败
 */
esp_err_t weather_icon_new_init(lv_obj_t *parent);

/**
 * @brief 显示指定天气代码的天气图标
 * 
 * 根据提供的天气代码显示对应的天气图标。
 * 从SPIFFS文件系统加载对应的PNG图标文件。
 * 
 * @param weather_code 天气代码字符串（如 "100", "101", "200" 等）
 */
void weather_icon_new_show(const char *weather_code);

/**
 * @brief 更新天气图标
 * 
 * 根据提供的天气代码更新天气图标。
 * 直接使用天气代码作为文件名，从SPIFFS文件系统加载对应的PNG图标。
 * 
 * @param weather_code 心知天气API返回的天气代码字符串
 */
void weather_icon_new_update(const char *weather_code);

/**
 * @brief 显示指定类型的天气图标
 * 
 * 根据提供的天气类型索引直接显示对应的天气图标。
 * 从SPIFFS文件系统加载对应的PNG图标。
 * 
 * @param type 天气类型索引（0-6）：
 *             0-晴天, 1-多云, 2-阴天, 3-雨天, 4-雪天, 5-雾天, 6-雷暴
 */
void weather_icon_new_show_type(int type);

/**
 * @brief 测试SPIFFS分区和天气图标文件
 * 
 * 用于诊断天气图标显示问题的测试函数。
 * 检查SPIFFS挂载状态、文件列表和文件读取。
 */
void weather_icon_test_spiffs(void);

/**
 * @brief 测试PNG解码器功能
 * 
 * 用于测试PNG解码器是否正常工作的测试函数。
 * 检查PNG解码器的初始化和解码功能。
 */
void weather_icon_test_png_decoder(void);

/**
 * @brief 反初始化天气图标显示模块
 * 
 * 清理天气图标显示相关的资源。
 * 在不再需要天气图标功能时调用此函数。
 */
void weather_icon_new_deinit(void);

/**
 * @brief 获取天气图标对象
 * 
 * 返回当前的天气图标对象，可用于进一步自定义或操作。
 * 
 * @return lv_obj_t* 天气图标对象，如果未初始化则返回NULL
 */
lv_obj_t* weather_icon_new_get_obj(void);

/**
 * @brief 测试C文件格式天气图标显示
 * 
 * 用于验证C数组格式的天气图标是否能正常显示的测试函数。
 * 直接使用转换好的C文件中的图像数据进行显示测试。
 */
void weather_icon_test_c_array(void);

#ifdef __cplusplus
}
#endif