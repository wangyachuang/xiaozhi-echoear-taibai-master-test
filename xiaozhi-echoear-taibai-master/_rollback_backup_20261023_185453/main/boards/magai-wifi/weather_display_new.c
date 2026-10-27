/**
 * @file weather_display_new.c
 * @brief 天气图标显示模块实现 - 新版本
 * 
 * 本模块提供天气图标显示相关的功能实现，使用SPIFFS文件系统加载PNG格式的天气图标文件。
 */

#include "weather_display_new.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <esp_spiffs.h>
#include "weather_icons_c/weather_icons.h"

#define TAG "weather_display_new"

// 全局变量
static lv_obj_t *weather_icon_obj = NULL;
static char weather_icon_path[64];         // PNG图标文件路径

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
esp_err_t weather_icon_new_init(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Initializing weather icon display module");
    
    // 创建天气图标对象 - 使用图像对象
    weather_icon_obj = lv_image_create(parent);
    if (weather_icon_obj == NULL) {
        ESP_LOGE(TAG, "Failed to create weather icon object");
        return ESP_FAIL;
    }
    
    // 设置图标位置和大小
    lv_obj_set_size(weather_icon_obj, 110, 110);  // 设置合适的大小
    lv_obj_align(weather_icon_obj, LV_ALIGN_CENTER, 0, 0);  // 居中对齐
    
    // 设置对象可见
    lv_obj_clear_flag(weather_icon_obj, LV_OBJ_FLAG_HIDDEN);
    
    ESP_LOGI(TAG, "Weather icon display module initialized successfully");
    return ESP_OK;
}

/**
 * @brief 显示指定天气代码的天气图标
 * 
 * 根据提供的天气代码显示对应的天气图标。
 * 从SPIFFS文件系统加载对应的PNG图标文件。
 * 
 * @param weather_code 天气代码字符串（如 "100", "101", "200" 等）
 */
void weather_icon_new_show(const char *weather_code)
{
    // 检查图标对象是否已初始化
    if (weather_icon_obj == NULL) {
        ESP_LOGE(TAG, "Weather icon object not initialized");
        return;
    }
    
    // 检查天气代码是否有效
    if (weather_code == NULL || strlen(weather_code) == 0) {
        ESP_LOGW(TAG, "Invalid weather code, using default sunny icon");
        weather_code = "100"; // 默认使用晴天代码
    }
    
    ESP_LOGI(TAG, "Displaying weather icon for code: %s", weather_code);
    
    // 构建PNG图标文件路径 (使用LVGL驱动器前缀 - 驱动器字母A对应ASCII 65)
    snprintf(weather_icon_path, sizeof(weather_icon_path), "A:/spiffs/%s.png", weather_code);
    
    // 检查文件是否存在 (使用POSIX路径格式)
    char posix_path[128];
    snprintf(posix_path, sizeof(posix_path), "/spiffs/%s.png", weather_code);
    FILE *file = fopen(posix_path, "r");
    if (file != NULL) {
        fclose(file);
        ESP_LOGI(TAG, "Found weather icon: %s", weather_icon_path);
        
        // 设置PNG图像源
        lv_image_set_src(weather_icon_obj, weather_icon_path);
        
        // 确保图标可见
        lv_obj_clear_flag(weather_icon_obj, LV_OBJ_FLAG_HIDDEN);
        // 移动到前景
        lv_obj_move_foreground(weather_icon_obj);
        // 强制刷新显示
        lv_obj_invalidate(weather_icon_obj);
        
        ESP_LOGI(TAG, "Weather icon loaded successfully for code: %s", weather_code);
    } else {
        ESP_LOGW(TAG, "Weather icon file not found: %s, trying fallback", weather_icon_path);
        
        // 尝试使用默认图标
        snprintf(weather_icon_path, sizeof(weather_icon_path), "A:/spiffs/100.png");
        snprintf(posix_path, sizeof(posix_path), "/spiffs/100.png");
        file = fopen(posix_path, "r");
        if (file != NULL) {
            fclose(file);
            lv_image_set_src(weather_icon_obj, weather_icon_path);
            ESP_LOGI(TAG, "Using fallback sunny icon");
        } else {
            ESP_LOGE(TAG, "No weather icons available in SPIFFS");
        }
    }
}

/**
 * @brief 更新天气图标
 * 
 * 根据提供的天气代码更新天气图标。
 * 直接使用天气代码作为文件名，从SPIFFS文件系统加载对应的PNG图标。
 * 
 * @param weather_code 心知天气API返回的天气代码字符串
 */
void weather_icon_new_update(const char *weather_code)
{
    // 检查图标对象是否已初始化
    if (weather_icon_obj == NULL) {
        ESP_LOGE(TAG, "Weather icon object not initialized");
        return;
    }
    
    // 检查天气代码是否有效
    if (weather_code == NULL || strlen(weather_code) == 0) {
        ESP_LOGW(TAG, "Invalid weather code, using default sunny icon");
        weather_code = "100"; // 默认使用晴天代码
    }
    
    ESP_LOGI(TAG, "Updating weather icon for code: %s", weather_code);
    
    // 天气代码映射：将API返回的简短代码映射到标准的三位数代码
    char mapped_code[10];
    if (strcmp(weather_code, "0") == 0 || strcmp(weather_code, "1") == 0) {
        strcpy(mapped_code, "100"); // 晴天
    } else if (strcmp(weather_code, "2") == 0) {
        strcpy(mapped_code, "101"); // 多云
    } else if (strcmp(weather_code, "3") == 0) {
        strcpy(mapped_code, "104"); // 阴天
    } else if (strlen(weather_code) < 3) {
        // 对于其他短代码，尝试补零到三位数
        snprintf(mapped_code, sizeof(mapped_code), "%03d", atoi(weather_code));
    } else {
        // 已经是三位数代码，直接使用
        strncpy(mapped_code, weather_code, sizeof(mapped_code) - 1);
        mapped_code[sizeof(mapped_code) - 1] = '\0';
    }
    
    ESP_LOGI(TAG, "Mapped weather code from %s to %s", weather_code, mapped_code);
    
    // 构建PNG图标文件路径 (使用LVGL驱动器前缀 - 驱动器字母A对应ASCII 65)
        snprintf(weather_icon_path, sizeof(weather_icon_path), "A:/spiffs/%s.png", mapped_code);
    
    // 检查文件是否存在 (使用POSIX路径格式)
    char posix_path[128];
    snprintf(posix_path, sizeof(posix_path), "/spiffs/%s.png", mapped_code);
    FILE *file = fopen(posix_path, "r");
    if (file != NULL) {
        fclose(file);
        ESP_LOGI(TAG, "Found weather icon: %s", weather_icon_path);
        
        // 设置PNG图像源
        ESP_LOGI(TAG, "Setting image source: %s", weather_icon_path);
        lv_image_set_src(weather_icon_obj, weather_icon_path);
        
        // 处理LVGL任务以确保图像加载
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(50)); // 等待50ms让图像加载
        
        // 检查图像是否加载成功
        const void* src = lv_image_get_src(weather_icon_obj);
        lv_coord_t w = lv_obj_get_width(weather_icon_obj);
        lv_coord_t h = lv_obj_get_height(weather_icon_obj);
        ESP_LOGI(TAG, "Image source: %p, size: %dx%d", src, (int)w, (int)h);
        
        // 应用图像优化设置
        // 注释掉自适应尺寸设置，保持固定尺寸
        // lv_obj_set_size(weather_icon_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);  // 设置为内容自适应尺寸
        lv_obj_set_style_img_recolor_opa(weather_icon_obj, LV_OPA_0, 0);  // 禁用重新着色
        lv_obj_set_style_transform_angle(weather_icon_obj, 0, 0);  // 确保角度为0
        lv_obj_set_style_transform_zoom(weather_icon_obj, 256, 0);  // 设置缩放为1.0倍
        
        // 确保图标可见
        lv_obj_clear_flag(weather_icon_obj, LV_OBJ_FLAG_HIDDEN);
        // 移动到前景
        lv_obj_move_foreground(weather_icon_obj);
        // 强制刷新显示
        lv_obj_invalidate(weather_icon_obj);
        
        // 再次检查尺寸
        w = lv_obj_get_width(weather_icon_obj);
        h = lv_obj_get_height(weather_icon_obj);
        ESP_LOGI(TAG, "Final image size: %dx%d", (int)w, (int)h);
        
        ESP_LOGI(TAG, "Weather icon updated successfully for code: %s", weather_code);
    } else {
        ESP_LOGW(TAG, "Weather icon file not found: %s, trying fallback", weather_icon_path);
        
        // 尝试使用默认图标
        snprintf(weather_icon_path, sizeof(weather_icon_path), "A:/spiffs/100.png");
        snprintf(posix_path, sizeof(posix_path), "/spiffs/100.png");
        file = fopen(posix_path, "r");
        if (file != NULL) {
            fclose(file);
            lv_image_set_src(weather_icon_obj, weather_icon_path);
            ESP_LOGI(TAG, "Using fallback sunny icon");
        } else {
            ESP_LOGE(TAG, "No weather icons available in SPIFFS");
        }
    }
}

/**
 * @brief 显示指定类型的天气图标
 * 
 * 根据提供的天气类型索引直接显示对应的天气图标。
 * 从SPIFFS文件系统加载对应的PNG图标。
 * 
 * @param type 天气类型索引（0-6）：
 *             0-晴天, 1-多云, 2-阴天, 3-雨天, 4-雪天, 5-雾天, 6-雷暴
 */
void weather_icon_new_show_type(int type)
{
    // 检查图标对象是否已初始化
    if (weather_icon_obj == NULL) {
        ESP_LOGE(TAG, "Weather icon object not initialized");
        return;
    }
    
    // 检查天气类型是否有效
    if (type < 0 || type > 6) {
        ESP_LOGW(TAG, "Invalid weather type: %d, using default sunny type", type);
        type = 0; // 默认使用晴天类型
    }
    
    // 天气类型对应的天气代码
    const char *weather_codes[] = {
        "100", // 晴天
        "104", // 多云
        "101", // 多云（原阴天）
        "300", // 雨天
        "400", // 雪天
        "500", // 雾天
        "302"  // 雷暴
    };
    
    // 构建PNG图标文件路径 (使用LVGL驱动器前缀 - 驱动器字母A对应ASCII 65)
    snprintf(weather_icon_path, sizeof(weather_icon_path), "A:/spiffs/%s.png", weather_codes[type]);
    
    // 检查文件是否存在 (使用POSIX路径格式)
    char posix_path[128];
    snprintf(posix_path, sizeof(posix_path), "/spiffs/%s.png", weather_codes[type]);
    FILE *file = fopen(posix_path, "r");
    if (file != NULL) {
        fclose(file);
        ESP_LOGI(TAG, "Found weather icon: %s", weather_icon_path);
        
        // 设置PNG图像源
        lv_image_set_src(weather_icon_obj, weather_icon_path);
        
        // 应用图像优化设置
        // 注释掉自适应尺寸设置，保持固定尺寸
        // lv_obj_set_size(weather_icon_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);  // 设置为内容自适应尺寸
        lv_obj_set_style_img_recolor_opa(weather_icon_obj, LV_OPA_0, 0);  // 禁用重新着色
        lv_obj_set_style_transform_angle(weather_icon_obj, 0, 0);  // 确保角度为0
        lv_obj_set_style_transform_zoom(weather_icon_obj, 256, 0);  // 设置缩放为1.0倍
        
        // 确保图标可见
        lv_obj_clear_flag(weather_icon_obj, LV_OBJ_FLAG_HIDDEN);
        // 移动到前景
        lv_obj_move_foreground(weather_icon_obj);
        // 强制刷新显示
        lv_obj_invalidate(weather_icon_obj);
        
        ESP_LOGI(TAG, "Loading weather icon from SPIFFS: %s with optimization", weather_icon_path);
    } else {
        ESP_LOGW(TAG, "Weather icon file not found: %s", weather_icon_path);
    }
}

/**
 * @brief 测试SPIFFS分区和天气图标文件
 * 
 * 用于诊断天气图标显示问题的测试函数。
 * 检查SPIFFS挂载状态、文件列表和文件读取。
 */
void weather_icon_test_spiffs(void)
{
    ESP_LOGI(TAG, "=== 开始SPIFFS天气图标诊断测试 ===");
    
    // 1. 检查SPIFFS分区信息
    size_t total = 0, used = 0;
    esp_err_t ret = esp_spiffs_info("weather", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取SPIFFS信息失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPIFFS分区信息: 总大小=%d KB, 已使用=%d KB, 可用=%d KB", 
             total/1024, used/1024, (total-used)/1024);
    
    // 2. 列出/spiffs目录下的文件
    ESP_LOGI(TAG, "列出/spiffs目录下的文件:");
    DIR *dir = opendir("/spiffs");
    if (dir == NULL) {
        ESP_LOGE(TAG, "无法打开/spiffs目录");
        return;
    }
    
    struct dirent *entry;
    int file_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) { // 只显示文件，不显示目录
            file_count++;
            ESP_LOGI(TAG, "  文件 %d: %s", file_count, entry->d_name);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "总共找到 %d 个文件", file_count);
    
    // 3. 测试常见天气图标文件是否存在
    ESP_LOGI(TAG, "测试常见天气图标文件:");
    const char* test_icons[] = {"100", "101", "104", "300", "400", "500", "302"};
    const char* icon_names[] = {"晴天", "多云", "阴天", "雨天", "雪天", "雾天", "雷暴"};
    
    for (int i = 0; i < 7; i++) {
        char filepath[64];
        snprintf(filepath, sizeof(filepath), "/spiffs/%s.png", test_icons[i]);
        
        FILE *file = fopen(filepath, "rb");
        if (file) {
            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fclose(file);
            ESP_LOGI(TAG, "  ✓ %s (%s): %ld 字节", icon_names[i], filepath, size);
        } else {
            ESP_LOGE(TAG, "  ✗ %s (%s): 文件不存在", icon_names[i], filepath);
        }
    }
    
    // 4. 验证PNG文件头
    FILE *file = fopen("/spiffs/100.png", "rb");
    if (file) {
        uint8_t header[8];
        size_t read_size = fread(header, 1, 8, file);
        fclose(file);
        
        if (read_size == 8) {
            ESP_LOGI(TAG, "100.png文件头: %02X %02X %02X %02X %02X %02X %02X %02X", 
                     header[0], header[1], header[2], header[3], 
                     header[4], header[5], header[6], header[7]);
            
            // PNG文件头应该是: 89 50 4E 47 0D 0A 1A 0A
            if (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
                ESP_LOGI(TAG, "✓ PNG文件头验证通过");
            } else {
                ESP_LOGE(TAG, "✗ PNG文件头验证失败");
            }
        }
    }
    
    ESP_LOGI(TAG, "=== SPIFFS天气图标诊断测试完成 ===");
}

/**
 * @brief 测试PNG解码器功能
 * 
 * 用于验证LVGL的PNG解码器是否正常工作的测试函数。
 * 创建临时图像对象来实际测试PNG解码和显示。
 */
void weather_icon_test_png_decoder(void)
{
    ESP_LOGI(TAG, "=== 开始PNG解码器测试 ===");
    
    // 测试LVGL是否能正确加载PNG图像
    const char* test_file_lvgl = "A:/spiffs/100.png";  // LVGL路径格式
    const char* test_file_posix = "/spiffs/100.png";   // POSIX路径格式
    
    // 检查文件是否存在（使用POSIX路径）
    FILE *file = fopen(test_file_posix, "rb");
    if (!file) {
        ESP_LOGE(TAG, "测试文件 %s 不存在", test_file_posix);
        return;
    }
    fclose(file);
    ESP_LOGI(TAG, "文件存在检查通过: %s", test_file_posix);
    
    // 验证PNG文件头
    file = fopen(test_file_posix, "rb");
    if (file) {
        uint8_t header[8];
        size_t read_size = fread(header, 1, 8, file);
        fclose(file);
        
        if (read_size == 8) {
            ESP_LOGI(TAG, "PNG文件头: %02X %02X %02X %02X %02X %02X %02X %02X", 
                     header[0], header[1], header[2], header[3], 
                     header[4], header[5], header[6], header[7]);
            
            // PNG文件头应该是: 89 50 4E 47 0D 0A 1A 0A
            if (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
                ESP_LOGI(TAG, "✓ PNG文件头验证通过，文件格式正确");
            } else {
                ESP_LOGE(TAG, "✗ PNG文件头验证失败");
                return;
            }
        }
    }
    
    // 实际测试LVGL PNG解码器
    ESP_LOGI(TAG, "开始LVGL PNG解码器实际测试...");
    
    // 获取当前活动屏幕
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "无法获取活动屏幕");
        return;
    }
    
    // 创建临时图像对象进行测试
    lv_obj_t* test_img = lv_image_create(screen);
    if (!test_img) {
        ESP_LOGE(TAG, "创建测试图像对象失败");
        return;
    }
    
    ESP_LOGI(TAG, "临时图像对象创建成功");
    
    // 设置图像位置（屏幕正中央，更明显的位置）
    lv_obj_set_pos(test_img, (LV_HOR_RES - 60) / 2, (LV_VER_RES - 60) / 2);
    lv_obj_set_size(test_img, 60, 60);
    
    // 设置高层级确保在最前面显示
    lv_obj_move_foreground(test_img);
    
    // 尝试加载PNG图像
    ESP_LOGI(TAG, "尝试加载PNG图像: %s", test_file_lvgl);
    lv_image_set_src(test_img, test_file_lvgl);
    
    // 确保图像可见并设置透明度
    lv_obj_clear_flag(test_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(test_img, LV_OPA_COVER, 0);
    lv_obj_move_foreground(test_img);
    
    // 处理LVGL任务以确保图像被处理
    for (int i = 0; i < 5; i++) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // 检查图像是否成功加载
    const void* src = lv_image_get_src(test_img);
    if (src) {
        ESP_LOGI(TAG, "✓ PNG图像源设置成功");
        
        // 获取图像实际尺寸
         lv_coord_t width = lv_obj_get_width(test_img);
         lv_coord_t height = lv_obj_get_height(test_img);
         ESP_LOGI(TAG, "图像尺寸: %dx%d", (int)width, (int)height);
        
        // 显示测试图像3秒钟
        ESP_LOGI(TAG, "测试图像将显示3秒钟，请观察屏幕中央是否有天气图标");
        vTaskDelay(pdMS_TO_TICKS(3000));
        
    } else {
        ESP_LOGE(TAG, "✗ PNG图像加载失败");
    }
    
    // 清理测试对象（使用延迟删除避免内存问题）
    ESP_LOGI(TAG, "清理测试对象...");
    lv_obj_delete_delayed(test_img, 100);  // 延迟100ms删除
    
    // 处理LVGL任务以确保删除操作完成
    for (int i = 0; i < 3; i++) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    ESP_LOGI(TAG, "=== PNG解码器测试完成 ===");
}

/**
 * @brief 反初始化天气图标显示模块
 * 
 * 清理天气图标显示相关的资源。
 * 在不再需要天气图标功能时调用此函数。
 */
void weather_icon_new_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing weather icon display module");
    
    // 清理路径缓冲区
    memset(weather_icon_path, 0, sizeof(weather_icon_path));
    
    // 注意：weather_icon_obj 由 LVGL 管理，不需要手动释放
    weather_icon_obj = NULL;
    
    ESP_LOGI(TAG, "Weather icon display module deinitialized");
}

/**
 * @brief 获取天气图标对象
 * 
 * 返回当前的天气图标对象，可用于进一步自定义或操作。
 * 
 * @return lv_obj_t* 天气图标对象，如果未初始化则返回NULL
 */
lv_obj_t* weather_icon_new_get_obj(void)
{
    // 返回天气图标对象指针
    return weather_icon_obj;
}

/**
 * @brief 测试C文件格式天气图标显示
 * 
 * 用于验证C数组格式的天气图标是否能正常显示的测试函数。
 * 直接使用转换好的C文件中的图像数据进行显示测试。
 */
void weather_icon_test_c_array(void)
{
    ESP_LOGI(TAG, "=== 开始C数组格式天气图标测试 ===");
    
    // 获取当前活动屏幕
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "无法获取活动屏幕");
        return;
    }
    
    // 创建临时图像对象进行测试
    lv_obj_t* test_img = lv_image_create(screen);
    if (!test_img) {
        ESP_LOGE(TAG, "创建测试图像对象失败");
        return;
    }
    
    ESP_LOGI(TAG, "临时图像对象创建成功");
    
    // 设置图像位置（屏幕正中央）
    lv_obj_set_pos(test_img, (LV_HOR_RES - 60) / 2, (LV_VER_RES - 60) / 2);
    lv_obj_set_size(test_img, 60, 60);
    
    // 设置高层级确保在最前面显示
    lv_obj_move_foreground(test_img);
    
    // 尝试加载C数组图像
    ESP_LOGI(TAG, "尝试加载C数组格式图像数据");
    
    // 使用头文件中定义的天气图标变量
    lv_image_set_src(test_img, &_icon_100);
    
    // 确保图像可见并设置透明度
    lv_obj_clear_flag(test_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(test_img, LV_OPA_COVER, 0);
    lv_obj_move_foreground(test_img);
    
    // 处理LVGL任务以确保图像被处理
    for (int i = 0; i < 5; i++) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // 检查图像是否成功加载
    const void* src = lv_image_get_src(test_img);
    if (src) {
        ESP_LOGI(TAG, "✓ C数组图像源设置成功");
        
        // 获取图像实际尺寸
        lv_coord_t width = lv_obj_get_width(test_img);
        lv_coord_t height = lv_obj_get_height(test_img);
        ESP_LOGI(TAG, "图像尺寸: %dx%d", (int)width, (int)height);
        
        // 显示测试图像3秒钟
        ESP_LOGI(TAG, "测试图像将显示3秒钟，请观察屏幕中央是否有天气图标");
        vTaskDelay(pdMS_TO_TICKS(3000));
        
    } else {
        ESP_LOGE(TAG, "✗ C数组图像加载失败");
    }
    
    // 清理测试对象（使用延迟删除避免内存问题）
    ESP_LOGI(TAG, "清理测试对象...");
    lv_obj_delete_delayed(test_img, 100);  // 延迟100ms删除
    
    // 处理LVGL任务以确保删除操作完成
    for (int i = 0; i < 3; i++) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    ESP_LOGI(TAG, "=== C数组格式天气图标测试完成 ===");
}