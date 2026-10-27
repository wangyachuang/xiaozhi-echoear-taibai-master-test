// ============================================================================
// lvgl_clock_probe.h — 第 1 步验证：panel IO 回调切换架构
// ----------------------------------------------------------------------------
// 目标（只验证架构，不改时钟页）：
//   证明"同一块屏在不同界面下切换绘制引擎"这条路走得通，即：
//     表情页：panel_io 的 on_color_trans_done 回调 = gfx 引擎
//     时钟页：同一个回调改指向 LVGL
//     来回切换不崩、表情不卡。
//
// 背景（已在 IDF 源码确认）：
//   esp_lcd_panel_io_register_event_callbacks() 内部是【覆盖】而非叠加
//   （esp_lcd_panel_io_spi.c:167-172，重复注册会打印 overwritten 警告）。
//   结构体里 on_color_trans_done / user_ctx 各只有一份。
//   因此"谁后注册谁生效"，这正好可以被用来做界面切换。
//
// 用法：
//   把 BOARD_LVGL_CLOCK_PROBE 改成 1 编译烧录，然后：
//     - 用你的滑动操作进入时钟页 -> 应看到 LVGL 画的青色测试圆弧
//     - 再滑回表情页              -> 表情应恢复正常（不卡）
//   观察串口 [LVGL_CLOCK_PROBE] 日志。
//   验证完把开关改回 0（默认），本文件即可整文件删除。
// ============================================================================
#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

// ★ 总开关：默认关闭，为 0 时全部代码被编译掉，不影响现有功能
#define BOARD_LVGL_CLOCK_PROBE 0

#if BOARD_LVGL_CLOCK_PROBE

#ifdef __cplusplus
extern "C" {
#endif

// 平台初始化：创建 LVGL 显示（挂到同一个 panel_io/panel）。
// 由 Board 在 EmoteDisplay 创建之后调用一次。
void LvglClockProbe_Init(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel);

// 切换到 LVGL 绘制（进入时钟页时调用）。
// 内部会把 panel_io 的完成回调改为 LVGL 的回调。
void LvglClockProbe_Enter(void);

// 切回 gfx 绘制（离开时钟页时调用）。
// 内部把回调重新注册回 gfx 引擎（由 EmoteDisplay 提供）。
void LvglClockProbe_Leave(void);

// 供 EmoteDisplay 把自己的回调注册函数暴露出来，用于"切回 gfx"。
// 参数 engine_handle 即 gfx 句柄。
void LvglClockProbe_SetGfxCallbackRegistrar(void (*fn)(void*), void* engine_handle);

#ifdef __cplusplus
}
#endif

#endif  // BOARD_LVGL_CLOCK_PROBE
