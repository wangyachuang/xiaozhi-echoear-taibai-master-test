// ============================================================================
// clock_page_lvgl.h — 时钟 App（复刻 Coze / ESP-Brookesia Timer 风格）
// ----------------------------------------------------------------------------
// 目标：对齐 espressif/esp-brookesia release/v0.6 的
//       apps/brookesia_app_timer（SquareLine 工程名 "Smartwatch"）。
//
// 官方界面的构成（我从它的 ui_Screen_watch_digital.h / analog.h 解析得到）：
//   数字页：
//     - 大号 HH : MM（官方用 100px 级 "Number_big" 字体）
//     - 电池组 Panel：图标 + "NN%" + 充电闪电
//     - 步数组 Panel：图标 + 步数
//     - Daily Mission 组：进度弧 + 百分比 + 图标
//     - 秒点 Image_sec_dot
//   模拟页：表盘指针
//   点按屏幕在 数字/模拟 间切换（官方 switchScreen）
//
// 本实现的取舍（受项目约束）：
//   * 不引入官方那两个约 270KB 的字体，用 BUILTIN_TEXT_FONT + transform
//     放大实现"大号时间"，零额外 flash。
//   * 步数/Daily Mission 你的项目里没有传感器数据源，保留位置但不显示，
//     等有数据再接（见 kShowPlaceholderGroups）。
//   * 电池图标用 battery_icon 的分档图，与官方 4 档 + 充电语义一致。
// ============================================================================
#pragma once

#include "sdkconfig.h"

#include <lvgl.h>

namespace clock_page_lvgl {

// 创建并返回时钟 App 页 screen。
lv_obj_t* Create();

// 生成/销毁该页界面；active 时启动 1Hz 定时器。
void SetActive(bool active);

// 手动刷新一次。
void Refresh();

}  // namespace clock_page_lvgl
