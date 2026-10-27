// ============================================================================
// app_launcher.h — LVGL 桌面（App 宫格）
// ----------------------------------------------------------------------------
// 交互模型（对齐 Coze / ESP-Brookesia 的 speaker 桌面）：
//   第 0 页 = gfx 表情页（面板由 gfx 独占，LVGL 不碰 SPI）
//   第 1 页 = 本桌面：2x2 App 宫格，点击图标进入对应 App
//   第 2 页起 = 各 App 自己的界面
//
// 与旧版的区别：
//   旧版是"横向滑动逐页翻"（表情页 -> 时钟页 -> ...），
//   现在是"表情页 <-> 桌面"两态，桌面内用点击进入 App，
//   和扣子喵伴的操作一致。
//
// App 图标是 64x64 的 LVGL 图片（RGB565A8），资源在 main/display/images/。
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

// 图标回调：点击某个 App 图标时被调用（参数是该 Entry 的 page_index）。
// 由 emote_app 注入，避免 app_launcher 反向依赖页面管理。
using LaunchCallback = void (*)(int page_index);
void SetLaunchCallback(LaunchCallback cb);

// 创建桌面页。必须在 LVGL 已初始化且持有 lvgl_port 锁时调用。
lv_obj_t* Create();

// 桌面页是否处于可见状态
bool IsActive();

// 由 emote_app::GotoPage 通知：进入/离开桌面。
void OnEnter();
void OnLeave();

// 注册 App。Create() 之前或之后都可以调用：
//   - Create() 之前：只记录到列表，宫格在 Create() 里一次性建好；
//   - Create() 之后：自动重建宫格（emote_app 采用这个顺序）。
void RegisterApp(const AppEntry& app);

// 原始触摸喂入（按下/抬起坐标）。只在桌面可见时消费点击。
// 返回 true 表示本次触摸已被桌面消费（调用方不要再做滑动判定）。
bool FeedTouch(bool pressed, int x, int y);

}  // namespace app_launcher
