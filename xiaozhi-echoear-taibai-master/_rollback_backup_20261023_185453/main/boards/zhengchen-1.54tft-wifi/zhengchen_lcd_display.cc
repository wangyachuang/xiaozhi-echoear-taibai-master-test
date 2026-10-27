#include "zhengchen_lcd_display.h"

#include <esp_log.h>

#include <algorithm>
#include <cstring>

#define TAG "ZhengchenLcdDisplay"

// 表情映射表 - 将原版21种表情映射到现有6个GIF
const ZhengchenLcdDisplay::EmotionMap ZhengchenLcdDisplay::emotion_maps_[] = {
    // 中性/平静类表情 -> staticstate
    {"neutral", &staticstate},
    {"relaxed", &staticstate},
    {"sleepy", &staticstate},

    // 积极/开心类表情 -> happy
    {"happy", &happy},
    {"laughing", &happy},
    {"funny", &happy},
    {"loving", &happy},
    {"confident", &happy},
    {"winking", &happy},
    {"cool", &happy},
    {"delicious", &happy},
    {"kissy", &happy},
    {"silly", &happy},

    // 悲伤类表情 -> sad
    {"sad", &sad},
    {"crying", &sad},

    // 愤怒类表情 -> anger
    {"angry", &anger},

    // 惊讶类表情 -> scare
    {"surprised", &scare},
    {"shocked", &scare},

    // 思考/困惑类表情 -> buxue
    {"thinking", &buxue},
    {"confused", &buxue},
    {"embarrassed", &buxue},

    {nullptr, nullptr}  // 结束标记
};

ZhengchenLcdDisplay::ZhengchenLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy, DisplayFonts fonts)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                    fonts),
      emotion_gif_(nullptr),
      high_temp_popup_(nullptr),
      high_temp_label_(nullptr) {
    ESP_LOGI(TAG, "初始化ZhengchenLcdDisplay");
    SetupGifContainer();
    // 立即显示一个默认表情
    ESP_LOGI(TAG, "初始化完成，显示默认表情");
    SetEmotion("neutral");
};

void ZhengchenLcdDisplay::SetupGifContainer() {
    ESP_LOGI(TAG, "开始设置GIF容器");
    DisplayLockGuard lock(this);

    if (emotion_label_) {
        lv_obj_del(emotion_label_);
        ESP_LOGI(TAG, "删除旧的emotion_label_");
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
        ESP_LOGI(TAG, "删除旧的chat_message_label_");
    }
    if (content_) {
        lv_obj_del(content_);
        ESP_LOGI(TAG, "删除旧的content_");
    }

    // 创建内容容器
    content_ = lv_obj_create(container_);
    if (!content_) {
        ESP_LOGE(TAG, "创建内容容器失败!");
        return;
    }
    ESP_LOGI(TAG, "创建内容容器成功");
    
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(content_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_center(content_);

    // 创建表情标签
    emotion_label_ = lv_label_create(content_);
    if (!emotion_label_) {
        ESP_LOGE(TAG, "创建表情标签失败!");
        return;
    }
    ESP_LOGI(TAG, "创建表情标签成功");
    
    lv_label_set_text(emotion_label_, "");
    lv_obj_set_width(emotion_label_, 0);
    lv_obj_set_style_border_width(emotion_label_, 0, 0);
    lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);

    // 创建GIF对象
    ESP_LOGI(TAG, "开始创建GIF对象");
    emotion_gif_ = lv_gif_create(content_);
    if (!emotion_gif_) {
        ESP_LOGE(TAG, "创建GIF对象失败!");
        return;
    }
    ESP_LOGI(TAG, "创建GIF对象成功");
    
    int gif_size = LV_HOR_RES;
    lv_obj_set_size(emotion_gif_, gif_size, gif_size);
    lv_obj_set_style_border_width(emotion_gif_, 0, 0);
    lv_obj_set_style_bg_opa(emotion_gif_, LV_OPA_TRANSP, 0);
    lv_obj_center(emotion_gif_);
    
    // 设置默认GIF表情
    ESP_LOGI(TAG, "设置默认GIF表情");
    lv_gif_set_src(emotion_gif_, &staticstate);

    // 创建聊天消息标签
    chat_message_label_ = lv_label_create(content_);
    if (!chat_message_label_) {
        ESP_LOGE(TAG, "创建聊天消息标签失败!");
        return;
    }
    ESP_LOGI(TAG, "创建聊天消息标签成功");
    
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_border_width(chat_message_label_, 0, 0);

    lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(chat_message_label_, 5, 0);

    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, 0);

    LcdDisplay::SetTheme("dark");
    ESP_LOGI(TAG, "GIF容器设置完成");
    
    // 添加一个测试消息
    lv_label_set_text(chat_message_label_, "表情显示测试");
    lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
}

void ZhengchenLcdDisplay::SetEmotion(const char* emotion) {
    if (!emotion) {
        ESP_LOGE(TAG, "emotion参数为空");
        return;
    }
    
    if (!emotion_gif_) {
        ESP_LOGE(TAG, "emotion_gif_对象不存在");
        return;
    }

    ESP_LOGI(TAG, "尝试设置表情: %s", emotion);
    DisplayLockGuard lock(this);

    for (const auto& map : emotion_maps_) {
        if (map.name && strcmp(map.name, emotion) == 0) {
            if (map.gif == NULL) {
                ESP_LOGE(TAG, "表情GIF资源不存在: %s", emotion);
                continue;
            }
            ESP_LOGI(TAG, "找到表情: %s, 设置GIF资源", emotion);
            lv_gif_set_src(emotion_gif_, map.gif);
            ESP_LOGI(TAG, "设置表情成功: %s", emotion);
            return;
        }
    }

    ESP_LOGI(TAG, "未找到匹配表情'%s'，使用默认表情", emotion);
    lv_gif_set_src(emotion_gif_, &staticstate);
    ESP_LOGI(TAG, "设置默认表情成功");
}

void ZhengchenLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    if (content == nullptr || strlen(content) == 0) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(chat_message_label_, content);
    lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "设置聊天消息 [%s]: %s", role, content);
}

void ZhengchenLcdDisplay::SetupHighTempWarningPopup() {
    DisplayLockGuard lock(this);
    
    // 创建高温警告弹窗
    high_temp_popup_ = lv_obj_create(lv_scr_act());  // 使用当前屏幕
    lv_obj_set_scrollbar_mode(high_temp_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(high_temp_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(high_temp_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(high_temp_popup_, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_radius(high_temp_popup_, 10, 0);
    
    // 创建警告标签
    high_temp_label_ = lv_label_create(high_temp_popup_);
    lv_label_set_text(high_temp_label_, "警告：温度过高");
    lv_obj_set_style_text_color(high_temp_label_, lv_color_white(), 0);
    lv_obj_center(high_temp_label_);
    
    // 默认隐藏
    lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
}

void ZhengchenLcdDisplay::UpdateHighTempWarning(float chip_temp, float threshold) {
    if (high_temp_popup_ == nullptr) {
        ESP_LOGW(TAG, "High temp popup not initialized!");
        return;
    }

    if (chip_temp >= threshold) {
        ShowHighTempWarning();
    } else {
        HideHighTempWarning();
    }
}

void ZhengchenLcdDisplay::ShowHighTempWarning() {
    DisplayLockGuard lock(this);
    if (high_temp_popup_ && lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ZhengchenLcdDisplay::HideHighTempWarning() {
    DisplayLockGuard lock(this);
    if (high_temp_popup_ && !lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
    }
} 