// ============================================================================
// ui_page.h — 页面基类
// ============================================================================
// 背景：
//   EchoEar 的屏幕此前由 gfx（表情引擎）独占，panel_io 的 on_color_trans_done
//   只有一份，谁最后注册谁拥有。现在要引入 LVGL 桌面（Coze 风格应用列表），
//   就必须让两套渲染后端"分时复用"同一块 panel：
//
//     gfx 模式  → 表情页（默认，开机即此页）
//     lvgl 模式 → launcher / app 等 LVGL 页面
//
//   所以这里抽象出一个"页面"概念：每个页面自己知道
//     - 我属于哪个后端（gfx 还是 lvgl）
//     - 进入 / 离开时我该做什么
//   PageManager 只负责编排"切后端 + 调页面生命周期"，不关心页面内部实现。
//
// 约定：
//   - 所有 OnEnter/OnExit/OnGesture 都在 PageManager 的临界区内被调用，
//     LVGL 侧已经在 lvgl_port_lock() 内，页面实现里无需再自己加锁。
//   - 页面不得在回调里同步阻塞超过 ~50ms，否则会卡住 LVGL 任务与触摸任务。
// ============================================================================
#pragma once

#include <lvgl.h>

namespace coze_ui {

// 渲染后端：决定 PageManager 把 panel 交给谁
enum class UIBackend {
    GFX,   // gfx（esp_emote_gfx）：表情页
    LVGL,  // esp_lvgl_port + LVGL：桌面/应用
};

// 手势方向（已由触摸层归一化，只保留主导轴）
enum class SwipeDir {
    LEFT,   // 手指向左划
    RIGHT,  // 手指向右划
};

class UiPage {
public:
    virtual ~UiPage() = default;

    // 该页面使用的后端。PageManager 依据它决定是否切换 panel 所有权。
    virtual UIBackend Backend() const = 0;

    // 调试用名字
    virtual const char* Name() const = 0;

    // 页面生命周期。首次进入时由页面自己惰性创建 LVGL 对象。
    virtual void OnEnter() = 0;
    virtual void OnExit() = 0;

    // 手势处理。返回 true 表示"我处理了，别再往上冒泡"。
    // 返回 false 时 PageManager 会回退到默认动作（左滑进桌面 / 右滑返回上级）。
    virtual bool OnGesture(SwipeDir dir)
    {
        (void)dir;
        return false;
    }

    // 说明：本基类【不】提供 OnTap()。点击完全交给 LVGL 自己的指针设备
    // （indev）判定并派发到具体控件，页面只需给控件挂 LV_EVENT_CLICKED。
    // 曾经有过一个 OnTap(x,y) 抽象，它与 indev 构成两条并行路径，
    // 会导致同一次点击被处理两遍，已移除。
};

}  // namespace coze_ui
