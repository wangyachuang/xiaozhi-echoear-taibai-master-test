// ============================================================================
// battery_icon.h — 电池图标分档（对齐 Coze / ESP-Brookesia 的状态栏语义）
// ----------------------------------------------------------------------------
// 参考实现：espressif/esp-brookesia release/v0.6
//          core/.../widgets/quick_settings/esp_brookesia_speaker_quick_settings.hpp
//
// 官方枚举：
//   enum class BatteryState {
//       CHARGING = -1, LEVEL_1, LEVEL_2, LEVEL_3, LEVEL_4, MAX
//   };
//   bool setBatteryPercent(bool charge_flag, int percent);
//
// 本文件把这套语义映射到 5 个 40×40 的 LVGL 图标（RGB565A8）：
//   charging        -> battery_charge
//   percent < 25    -> battery_level1
//   percent < 50    -> battery_level2
//   percent < 75    -> battery_level3
//   其它            -> battery_level4
//
// 图标资源在 main/display/images/*.c，共约 23.5KB flash。
// ============================================================================
#pragma once

#include <lvgl.h>

namespace battery_icon {

// 与官方 BatteryState 对应的档位（MAX 仅作哨兵）
enum class State {
    CHARGING = -1,
    LEVEL_1 = 0,
    LEVEL_2,
    LEVEL_3,
    LEVEL_4,
    MAX,
};

// 由"是否充电 + 百分比"算出档位。
// percent < 0 视为未知，返回 LEVEL_1（调用方可自行隐藏图标）。
State FromPercent(bool charging, int percent);

// 取对应图标描述符。未知时返回 nullptr。
const lv_image_dsc_t* GetImage(State state);

// 便捷：直接由充电标志与百分比取图标。
const lv_image_dsc_t* ImageFor(bool charging, int percent);

}  // namespace battery_icon
