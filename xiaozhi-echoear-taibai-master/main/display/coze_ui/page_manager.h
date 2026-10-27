// ============================================================================
// page_manager.h — 页面管理器（gfx 表情页 ⇄ LVGL 桌面/应用 的后端切换中枢）
// ============================================================================
// 设计目标（对应需求）：
//   1. 主界面仍是 gfx 表情页，开机即是它；
//   2. 手指向左滑 → 切到 LVGL 桌面；
//   3. 在桌面上时，表情必须"静默"，不占 CPU/SPI 带宽；
//   4. 页面管理独立成文件，后续加新界面只需实现 UiPage 并 register。
//
// 关键实现（为什么这样切后端最安全）：
//   panel_io 的 on_color_trans_done 只有一个槽位，gfx 与 esp_lvgl_port 谁后注册
//   谁拥有它。历史上"让 LVGL 抢走回调"的做法导致 gfx 永远等不到 flush 完成、
//   整屏折叠 + watchdog 复位。因此这里不抢回调，而是采用两条互补的静默手段：
//
//     A) gfx 侧：EmoteDisplay::SetGfxFlushOverride(非空) —— gfx 照常走完
//        渲染管线但不发起任何 SPI 传输（flush 被转发丢弃），回调仍归 gfx。
//     B) LVGL 侧：lvgl_port_stop() 停掉 LVGL 任务；lvgl_port_resume() 恢复。
//
//   切到桌面：先让 gfx 静默 → 再 resume LVGL（真正开始刷屏）；
//   回到表情：先 stop LVGL（不再写 panel）→ 再让 gfx 恢复真实 flush。
//   两边始终保证"同一时刻只有一个后端在写 SPI"。
//
// 锁序（重要，防止死锁）：
//   lvgl_port_lock 必须在外层，gfx lock 在内层。
//   因为 lvgl 的渲染任务是"先拿 lvgl 锁，再生成 flush 数据"，而 gfx 任务只拿
//   gfx 锁。若反过来先拿 gfx 锁再去等 lvgl 锁，LVGL 渲染任务可能在等待 gfx
//   完成信号时被卡住，形成环。PageManager 的 Init/Enter/Exit 全部遵循此序。
// ============================================================================
#pragma once

#include "ui_page.h"

#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <vector>
#include <string>

// LVGL 类型：这里只用指针作成员，前置声明即可，
// 避免把 lvgl.h 扩散到每个包含本头文件的编译单元（board 层也会包含它）。
struct _lv_display_t;
struct _lv_obj_t;
struct _lv_indev_t;

namespace emote {
class EmoteDisplay;
}

namespace coze_ui {

class PageManager {
public:
    static PageManager& GetInstance();

    // 必须在 EmoteDisplay 构造完成之后调用：
    //   - 记录 panel / panel_io / EmoteDisplay
    //   - 初始化 LVGL（lv_init + lvgl_port_init），但【先不挂 display】
    //   - 注册内置页面（launcher，以及 launcher 里列出的 app）
    //
    // 采用"延迟建 display"策略：开机时 LVGL 不占 DMA 缓冲、不刷一帧，
    // 表情页启动零干扰；第一次左滑进桌面时才调用 lv_display_create()。
    // 注意：这里刻意【不用】lvgl_port_add_disp()——它会无条件重注册
    // panel_io 的 on_color_trans_done，把 gfx 的唯一回调顶掉（历史事故）。
    // 代价是首次进桌面有一次性建屏开销（约几十 ms~200ms）。
    esp_err_t Init(emote::EmoteDisplay* display,
                   esp_lcd_panel_io_handle_t panel_io,
                   esp_lcd_panel_handle_t panel,
                   int width, int height);

    // 页面注册（按注册顺序进 launcher 图标表；PageManager 自己持有所有权）
    void RegisterPage(UiPage* page);

    // 触摸状态推送（由板级触摸任务调用）。
    // 之所以不让 esp_lvgl_port 自己扫 I2C：CST816S 由触摸任务独占读取，
    // 两处同时读同一颗 I2C 器件会互相打断（读到半包数据）。
    // 所以这里用"外部喂点"的方式：触摸任务读完寄存器后，把归一化坐标交给 LVGL。
    void FeedTouch(int x, int y, bool pressed);

    // 当前触摸电平（手指是否按在屏幕上）。
    // 板级触摸任务的 CheckTouchEvent() 会返回 TOUCH_NONE（本次无变化），
    // 这种情形必须沿用上一次的电平，不能凭空制造按下/松开边沿，
    // 否则 LVGL 侧会永远收不到 RELEASED，CLICKED 也就不成立。
    bool IsTouchPressed() const { return touch_pressed_; }

    // 当前是否处于 LVGL 页面（= 已离开表情页）
    bool InLvglMode() const { return in_lvgl_; }

    // 触摸任务入口：LR 手势统一从这里进
    // 必须在任何任务上下文中调用（内部只做加锁 + 状态机）
    //
    // 注意：点击【不】走这里。点击由 LVGL 自己的指针设备（indev）判定并派发
    // 到具体控件，板级触摸任务只负责 FeedTouch() 喂点。若再提供一个
    // HandleTap()，同一次点击会被两条路径各处理一遍。
    void HandleSwipe(SwipeDir dir);

    // ---- 供 launcher 回调的导航入口 ----
    bool OpenApp(UiPage* app);   // 从桌面进入某个 app
    bool BackToLauncher();       // app 内返回桌面
    bool BackToEmote();          // 桌面/任意 LVGL 页返回表情页

    // 由 EmoteDisplay 在构造后注入：让 gfx 静默 / 恢复
    // 之所以用函数指针而不直接持有 EmoteDisplay：
    // 避免 display 层反向依赖 coze_ui 头文件造成循环包含。
    using GfxMuteFn = void (*)(void* ctx, bool mute);
    void SetGfxMuteHook(GfxMuteFn fn, void* ctx);

    // 电池读数提供者（EmoteDisplay 注入，避免 coze_ui 反向包含板级头文件）
    using BatteryFn = int (*)(void* ctx);
    void SetBatteryHook(BatteryFn fn, void* ctx);

    // 注意：这两个访问器【不能】叫 LauncherPage()/TimerPage() 之类的名字。
    // 类成员名会遮蔽同名的全局类名（我们真有 coze_ui::LauncherPage 这个类），
    // 于是在 PageManager 成员函数里写 `new LauncherPage()` 会被解析成
    // "调用成员函数"，编译器报 "expected type-specifier before 'LauncherPage'"。
    UiPage* Launcher() const { return launcher_; }
    UiPage* Current() const { return current_; }

    // 当前页面名字，专供日志使用。没有当前页（表情页）时返回 "emote"。
    // 调试阶段加：用户反馈"什么都看不到变化"时，靠这一行确认到底停在哪一页。
    const char* CurrentPageName() const {
        return current_ != nullptr ? current_->Name() : "emote";
    }

    // 电池百分比（0~100），-1 表示未知。
    // 数据源与表情页共用 EmoteDisplay 里已有的 BQ27220 读数，不再单独读一次
    // I2C —— 两处各读一次既浪费总线也容易读到不一致的值。
    int BatteryPercent() const;

private:
    PageManager() = default;
    ~PageManager();
    PageManager(const PageManager&) = delete;
    PageManager& operator=(const PageManager&) = delete;

    // 惰性建 display：首次进 LVGL 页面时调用
    bool EnsureLvglDisplay();
    // 真正执行后端切换（调用方已持有 lvgl 锁）
    void SwitchToLvgl();
    void SwitchToGfx();
    // 切换当前页面。GotoPage 自己加 LVGL 锁（递归锁，已在锁内再进也安全），
    // 实际工作在 GotoPageLocked 里。返回 false 表示没有发生切换。
    bool GotoPage(UiPage* next);
    bool GotoPageLocked(UiPage* next);

    emote::EmoteDisplay* display_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    lv_display_t* lv_disp_ = nullptr;
    lv_obj_t* lv_root_ = nullptr;   // 所有页面共用的根 object
    lv_indev_t* lv_indev_ = nullptr;  // 由触摸任务喂点的指针设备

    // 最近一次 FeedTouch 的电平。TOUCH_NONE 时板级任务会读这个值沿用状态。
    bool touch_pressed_ = false;

    std::vector<UiPage*> pages_;
    UiPage* launcher_ = nullptr;
    UiPage* current_ = nullptr;

    bool in_lvgl_ = false;
    bool lvgl_ready_ = false;       // lv_init + lvgl_port_init 成功
    bool lvgl_ticking_ = false;     // lvgl_port_resume 状态
    bool inited_ = false;

    GfxMuteFn gfx_mute_ = nullptr;
    void* gfx_mute_ctx_ = nullptr;

    BatteryFn battery_ = nullptr;
    void* battery_ctx_ = nullptr;

    // 串行化"切后端"这一系列非可重入操作（触摸任务与其它调用方并发时）
    SemaphoreHandle_t mutex_ = nullptr;
};

}  // namespace coze_ui
