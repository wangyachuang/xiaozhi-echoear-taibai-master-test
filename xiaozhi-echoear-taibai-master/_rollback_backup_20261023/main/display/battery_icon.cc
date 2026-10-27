// ============================================================================
// battery_icon.cc — 电池图标分档实现
// ============================================================================
#include "battery_icon.h"

#if CONFIG_EMOTE_AS_LVGL_APP

// 图片资源由 main/display/images/*.c 提供（RGB565A8，40x40）
LV_IMAGE_DECLARE(img_battery_level1_40_40);
LV_IMAGE_DECLARE(img_battery_level2_40_40);
LV_IMAGE_DECLARE(img_battery_level3_40_40);
LV_IMAGE_DECLARE(img_battery_level4_40_40);
LV_IMAGE_DECLARE(img_battery_charge_40_40);

namespace battery_icon {

State FromPercent(const bool charging, const int percent)
{
    if (charging) {
        return State::CHARGING;
    }
    if (percent < 0) {
        // 未知电量：给最保守的一档，调用方通常会在此时隐藏图标
        return State::LEVEL_1;
    }
    if (percent < 25) {
        return State::LEVEL_1;
    }
    if (percent < 50) {
        return State::LEVEL_2;
    }
    if (percent < 75) {
        return State::LEVEL_3;
    }
    return State::LEVEL_4;
}

const lv_image_dsc_t* GetImage(const State state)
{
    switch (state) {
    case State::CHARGING:
        return &img_battery_charge_40_40;
    case State::LEVEL_1:
        return &img_battery_level1_40_40;
    case State::LEVEL_2:
        return &img_battery_level2_40_40;
    case State::LEVEL_3:
        return &img_battery_level3_40_40;
    case State::LEVEL_4:
        return &img_battery_level4_40_40;
    default:
        return nullptr;
    }
}

const lv_image_dsc_t* ImageFor(const bool charging, const int percent)
{
    return GetImage(FromPercent(charging, percent));
}

}  // namespace battery_icon

#endif  // CONFIG_EMOTE_AS_LVGL_APP
