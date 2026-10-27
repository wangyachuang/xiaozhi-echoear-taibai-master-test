// ============================================================================
// app_launcher.h — LVGL 桌面（App 宫格）
// ----------------------------------------------------------------------------
// 交互模型（对齐 Coze / ESP-Brookesia 的 speaker 桌面）：
//   第 0 页 = gfx 表情页（面板由 gfx 独占）
//   第 1 页 = 本桌面：2x2 App 宫格，点击图标进入对应 App
//   第 2 页起 = 各 App 自己的界面
//
// 与旧版的区别：
//   旧版是"横向滑动逐页翻"（表情页 -> 时钟页 -> ...），
//   现在是"表情页 <-> 桌面"两态，桌面内用点击进入 App，
//   和扣子喵伴的操作一致。
//
// App 图标用 64x64 的 LVGL 图片（RGB565A8），
// 图标资源由 main/display/images/ 提供，App 注册见 app_launcher.cc。
// ============================================================================
#pragma once

#include "sdkconfig.h"

#include <lvgl.h>

namespace app_launcher {

// 一个可启动的 App
struct AppEntry {
    const char* name;            // 显示在图标下方
    const lv_image_dsc_t* icon;  // 64x64 图标
    int page_index;              // 对应 emote_app 的页面下标
};

// 创建（并注册为第 1 页）桌面。返回该页 screen。
// 必须在 emote_app::InitBackend() 之后、注册各 App 页面之后调用。
lv_obj_t* Create();

// 桌面页是否可见
bool IsActive();

// 供 Board 触摸任务调用：判断一次点击是否落在某个 App 图标上。
// pressed=1 记录按下点，pressed=0 抬起时若位移很小则视为点击并跳转。
void FeedTouch(int pressed, int x, int y);

// 注册 App（会刷新宫格）。必须在 Create() 之前调用。
void RegisterApp(const AppEntry& app);

}  // namespace app_launcher
