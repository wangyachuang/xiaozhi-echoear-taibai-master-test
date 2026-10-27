// ============================================================================
// launcher_page.h — Coze 风格应用网格（桌面）
// ============================================================================
// 布局参数 1:1 照搬 esp-brookesia speaker 的 360x360 深色样式表
//   (core/brookesia_core/systems/speaker/stylesheets/360x360/dark/app_launcher_data.hpp)：
//
//   main      : 全屏，y_start = 0
//   table     : 3 列，高 50%，居中
//   icon.main : 140x140，纵向 flex，image 与 label 间距 pad_row = 10
//   icon.image: 98x98（按下 88x88）
//   icon.label: montserrat 16，白色
//   indicator : 底部 -30，未激活 12x12 / 激活 40x12，间距 10
//
// 交互（与 Coze 一致）：
//   - 按下 → 图标缩到 88x88；松开复原到 98x98；点击 → 打开对应 app。
//   - 桌面上右滑 → 回表情页（由 PageManager 默认动作处理）。
// ============================================================================
#pragma once

#include "ui_page.h"

#include <vector>
#include <string>

namespace coze_ui {

class LauncherPage : public UiPage {
public:
    struct Entry {
        const lv_image_dsc_t* icon = nullptr;  // 可为空：无图时只显示首字母
        const char* name = nullptr;
        UiPage* target = nullptr;              // 点击后进入的页面
    };

    LauncherPage() = default;
    ~LauncherPage() override;

    UIBackend Backend() const override { return UIBackend::LVGL; }
    const char* Name() const override { return "Launcher"; }

    void OnEnter() override;
    void OnExit() override;
    bool OnGesture(SwipeDir dir) override;

    void AddApp(const Entry& entry);

    // 按名字查找内建图标资源（Coze 的 app_launcher_icon 用 image 描述符打开 app）。
    // 目前只有 Timer 有真图（main/display/images/img_app_timer_64_64.c，64x64
    // RGB565A8），BuildIcons 会把它缩放进 98x98 的槽位。查不到返回 nullptr，
    // 此时退回首字母占位方块，保证图标一定可见。
    static const lv_image_dsc_t* FindBuiltinIcon(const char* name);

private:
    void Build();          // 惰性创建全部 LVGL 对象
    void BuildIcons();
    void UpdateIndicator();

    // 图标事件回调（与 Coze 的 app_launcher_icon.cpp 行为一一对应）
    static void PressedCb(lv_event_t* e);
    static void PressLostCb(lv_event_t* e);
    static void ReleasedCb(lv_event_t* e);
    static void ClickedCb(lv_event_t* e);

    std::vector<Entry> entries_;
    lv_obj_t* root_ = nullptr;        // 本页根容器（挂在 PageManager 的 lv_root_ 下）
    lv_obj_t* table_ = nullptr;       // 图标表格
    lv_obj_t* indicator_ = nullptr;   // 底部指示点容器
    std::vector<lv_obj_t*> icons_;
    std::vector<lv_obj_t*> dots_;
    bool built_ = false;
    int active_page_ = 0;
};

}  // namespace coze_ui
