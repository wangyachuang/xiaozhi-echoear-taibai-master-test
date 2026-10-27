// ============================================================================
// emote_app.h — 把 gfx 表情界面变成 LVGL 桌面里的"表情应用"
// ----------------------------------------------------------------------------
// 总开关：CONFIG_EMOTE_AS_LVGL_APP（main/Kconfig.projbuild）
//
// 设计说明见 emote_app.cc 顶部注释。核心是：
//   - 表情页由 gfx 直接画在 panel 上，LVGL 只负责该页的角标层；
//   - 其他页由 LVGL 画，此时 gfx 被"挂起"（flush 覆盖为空实现）；
//   - 两个引擎之间通过重新注册 panel_io 的 on_color_trans_done 交接。
//
// 用法（Board 侧）：
//   emote_app::InitBackend(panel_io, panel, emote_display);  // 开机自启
//   // 触摸任务里：
//   emote_app::FeedTouch(pressed, x, y);                     // 滑动切页
//   // 新应用页：
//   lv_obj_t* page = lv_obj_create(NULL);
//   ... 画 UI ...
//   emote_app::RegisterPage(page, "clock");
// ============================================================================
#pragma once

#include "sdkconfig.h"   // CONFIG_EMOTE_AS_LVGL_APP

#include "board.h"
#include "emote_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <lvgl.h>

namespace emote_app {

// ---------------------------------------------------------------------------
// 1) 后端切换：gfx <-> LVGL
// ---------------------------------------------------------------------------

// 初始化 LVGL + 桌面外壳。必须在 EmoteDisplay 构造完成之后调用。
void InitBackend(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                 emote::EmoteDisplay* emote);

// 切到 gfx 表情绘制（把 panel 回调与 gfx 的真实 flush 都装回去）。
void ActivateGfx();

// 切到 LVGL 绘制（gfx 的 flush 换成空实现，panel 回调交给 LVGL）。
void ActivateLvgl();

// ---------------------------------------------------------------------------
// 2) 桌面外壳：页面管理
// ---------------------------------------------------------------------------

// 寄存器页面。page 必须由 lv_obj_create(NULL) 创建（独立 screen）。
// 表情页由 InitBackend() 内部创建并固定为第 0 页。
//
// on_enter / on_leave 是该页的生命周期回调（可为 nullptr）：
//   on_enter: 页面变为可见时调用（启动该页的定时器、刷新一帧）
//   on_leave: 页面被隐藏前调用（停表，避免后台空转）
void RegisterPage(lv_obj_t* page, const char* name,
                  void (*on_enter)() = nullptr,
                  void (*on_leave)() = nullptr);

// 创建(并注册)表情页，返回该页。由 InitBackend() 自动调用，
// 通常无需手动使用；单独暴露便于把第 0 页换成自定义包装。
lv_obj_t* CreateEmotePage(emote::EmoteDisplay* emote);

// 页面数量（含表情页）
int PageCount();

// 跳转到指定页；越界或原地跳转会直接返回。
void GotoPage(int index);

// 等价于 GotoPage(cur+1) / GotoPage(cur-1)
void NextPage();
void PrevPage();

// ---------------------------------------------------------------------------
// 3) 状态查询 / 触摸喂入
// ---------------------------------------------------------------------------

// 当前是否停在表情页（即 gfx 正在绘制）。
bool IsEmotePageActive();

// Board 的触摸任务每次拿到坐标后调用。内部自行判断"横向滑动"，
// 竖向或短距离位移会直接忽略，交回 Board 原有手势逻辑。
void FeedTouch(int pressed, int x, int y);

// 预留：如需 LVGL 自己读触摸（会抢 Board 手势），再实现。
void AttachTouch(void* esp_lcd_touch_handle);

// 便捷方式：创建"表情页 + 内置应用页"的完整桌面。
// 目前内置 1 个应用页：时钟（数字/模拟，点击切换）。
// 在 Board 里只需调用这一个函数即可得到多页桌面。
void InitWithBuiltinApps(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         emote::EmoteDisplay* emote);

// ---------------------------------------------------------------------------
// 4) Coze 式桌面：表情页 <-> App 宫格
// ---------------------------------------------------------------------------
// 与 InitWithBuiltinApps 的区别：
//   旧模式：表情页 -> 时钟页 -> ... 横向逐页翻
//   新模式：表情页 <-> 桌面（第 1 页），桌面内点击图标进入 App
//
// 页面下标约定：
//   0 = gfx 表情页
//   1 = App 宫格桌面（app_launcher::Create 创建）
//   2.. = 各 App 自己的页面
void InitCozeStyleDesktop(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                          emote::EmoteDisplay* emote);

// 桌面宫格页是否可见（即当前停在桌面上）。
bool IsLauncherActive();

}  // namespace emote_app
