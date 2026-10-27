#pragma once

#include <lvgl.h>
#include <esp_log.h>
#include <cstring>

// 声明表情GIF
LV_IMAGE_DECLARE(staticstate);  // 静态状态/中性表情
LV_IMAGE_DECLARE(sad);          // 悲伤
LV_IMAGE_DECLARE(happy);        // 开心
LV_IMAGE_DECLARE(scare);        // 惊吓/惊讶
LV_IMAGE_DECLARE(buxue);        // 不学/困惑
LV_IMAGE_DECLARE(anger);        // 愤怒

/**
 * @brief 根据表情名称获取对应的GIF图像资源
 * 
 * @param name 表情名称
 * @return const lv_img_dsc_t* GIF图像资源，如果不存在则返回nullptr
 */
inline const lv_img_dsc_t* otto_emoji_gif_get_by_name(const char* name) {
    if (!name) return nullptr;

    // 中性/平静类表情
    if (strcmp(name, "neutral") == 0 || 
        strcmp(name, "relaxed") == 0 || 
        strcmp(name, "sleepy") == 0) {
        return &staticstate;
    }
    
    // 开心类表情
    if (strcmp(name, "happy") == 0 || 
        strcmp(name, "laughing") == 0 || 
        strcmp(name, "funny") == 0 ||
        strcmp(name, "loving") == 0 ||
        strcmp(name, "confident") == 0 ||
        strcmp(name, "winking") == 0 ||
        strcmp(name, "cool") == 0 ||
        strcmp(name, "delicious") == 0 ||
        strcmp(name, "kissy") == 0 ||
        strcmp(name, "silly") == 0) {
        return &happy;
    }
    
    // 悲伤类表情
    if (strcmp(name, "sad") == 0 || 
        strcmp(name, "crying") == 0) {
        return &sad;
    }
    
    // 愤怒类表情
    if (strcmp(name, "angry") == 0) {
        return &anger;
    }
    
    // 惊讶类表情
    if (strcmp(name, "surprised") == 0 || 
        strcmp(name, "shocked") == 0) {
        return &scare;
    }
    
    // 思考/困惑类表情
    if (strcmp(name, "thinking") == 0 || 
        strcmp(name, "confused") == 0 || 
        strcmp(name, "embarrassed") == 0) {
        return &buxue;
    }
    
    // 默认返回中性表情
    return &staticstate;
} 