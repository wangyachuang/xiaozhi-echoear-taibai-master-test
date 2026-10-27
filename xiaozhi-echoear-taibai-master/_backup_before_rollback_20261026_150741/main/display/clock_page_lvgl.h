// ============================================================================
// clock_page_lvgl.h — Timer App（扣子喵伴同名的时钟应用）
// ----------------------------------------------------------------------------
// 只做「骨架优先」方案里约定的最小形态：
//   数字面：HH:MM（矢量化数字）+ 日期 + 电量 + 秒点
//   模拟面：点击切换
//   回桌面：下滑
//
// 为什么数字用 LVGL 矢量图元而不是字体：
//   当前 sdkconfig 只开了 CONFIG_LV_FONT_MONTSERRAT_14，没有 24/32/48 等大号
//   字体；就算打开，一套 0-9 的数字字形也要几 KB~几十 KB flash，而本工程
//   app 分区只剩约 466KB。用 arc + line 画七段/a-segment 数字 + 模拟表盘，
//   flash 开销接近 0，还能做圆角效果。
// ============================================================================
#pragma once

#include <lvgl.h>

namespace clock_page_lvgl {

// 创建 Timer 页（独立 screen，默认隐藏）。需持有 lvgl_port 锁。
lv_obj_t* Create();

// 进入/离开：进入时开 1Hz 定时器并立即刷一帧，离开时停表。
void SetActive(bool active);
bool IsActive();

// 数字面 <-> 模拟面（扣子 Timer 里点一下屏就切）
void ToggleFace();

// 对外只读：给 emote_app / Board 判断"现在在 Timer 里"
bool IsAnalogFace();

}  // namespace clock_page_lvgl
