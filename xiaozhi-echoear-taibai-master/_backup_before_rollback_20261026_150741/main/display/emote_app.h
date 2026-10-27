// ============================================================================
// emote_app.h — 把 gfx 表情界面变成 LVGL 桌面系统里的"表情页"
// ----------------------------------------------------------------------------
// 页面模型（对齐 Coze / ESP-Brookesia speaker）：
//   第 0 页 = gfx 表情页（面板由 gfx 独占；LVGL 只在最上层画角标）
//   第 1 页 = LVGL 桌面（App 宫格）
//   第 2 页起 = 各 App 界面（Timer ...）
//
// 手势：
//   表情页 横向滑  -> 桌面
//   桌面   横向滑  -> 表情页
//   App    下滑    -> 桌面
//
// 调用顺序（Board 侧）：
//   emote_app::InitCozeStyleDesktop(panel_io, panel, emote);
//   Board 的触摸任务里调用 emote_app::FeedTouch(...)
// ============================================================================
#pragma once

#include "sdkconfig.h"

#include <lvgl.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_types.h>

#include "emote_display.h"

namespace emote_app {

// 说明：这些声明必须放在 emote_app.cc 的「匿名命名空间（辅助函数）」之前。
// emote_app.cc 通过 #include "emote_app.h" 引入本头文件，因此在这里声明的
// 符号对文件内所有后续代码可见。若把 GotoPage 的声明留在文件末尾的
// namespace emote_app { ... } 块里，前面的辅助函数就看不到它，会报
// "'GotoPage' was not declared in this scope"。

// 初始化整套页面系统（含 LVGL 端口、桌面、Timer）。默认停在表情页。
void InitCozeStyleDesktop(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                          emote::EmoteDisplay* emote);

// 是否初始化成功（失败时 Board 应退回纯 gfx 表情模式）
bool IsReady();

// 把面板完全交还给 gfx（表情页），LVGL 进 no-op 模式。
// InitCozeStyleDesktop 失败时由 Board 调用，用于恢复到"没有 LVGL 参与"的状态。
void ForceGfxOnly();

// 原始触摸坐标（屏幕像素）。pressed=true 为按下，false 为抬起/离开。
// 返回 true 表示这次触摸已被页面系统消费（调用方不要再触发对话/滑动）。
bool FeedTouch(bool pressed, int x, int y);

// 当前页面
int CurrentPage();
bool IsEmotePageActive();
bool IsLauncherActive();

// 页面跳转（0 = 表情页）
void GotoPage(int index);

// 供 emote_display/gfx 侧查询：gfx 是否持有面板
bool IsGfxActive();

}  // namespace emote_app
