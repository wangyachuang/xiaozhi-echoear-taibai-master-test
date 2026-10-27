// ============================================================================
// timer_page.h — Timer 应用（数字表盘 + 模拟指针表盘）
// ============================================================================
// 移植自 esp-brookesia 的 apps/brookesia_app_timer：
//   原实现用两屏 SquareLine（数字表盘 / 模拟表盘）+ 1s esp_timer 刷新，
//   点击表盘切换两屏。这里复刻同样的交互。
//
// 数字表盘的每个元素、坐标、字号、颜色都对照
// apps/brookesia_app_timer/ui/screens/ui_Screen_watch_digital.c 一比一移植：
//   hour     montserrat_40 白 0xFFFFFF，屏幕 (100,120)，transform_scale 500
//   minute   montserrat_40 红 0xEE1E1E，屏幕 (210,140)，transform_scale 400
//   year     montserrat_20 白，CENTER+( 90,-78)
//   month    montserrat_24 白，CENTER+( 20,-80)   显示 "18 FEB"
//   weekday  montserrat_30 白，CENTER+(-70,-80)
//   电量组   77x22，TOP_MID+(0,20)，透明底 + 黑边框，flex ROW
//   步数组   100x26，BOTTOM_MID+(0,-40)，flex ROW / END 对齐
//   任务组   283x185，BOTTOM_LEFT，330x330 arc，88..170 度，色 0xB0E14A
//   sec_dot  CENTER+(10,15)，pivot(4,194)，每秒转 6 度
//
// 与原版的两处差别（都不影响外观，只影响资源使用）：
//   1) 原版用 PNG：flash 图标 / step 图标 / daily_mission 图标 / sec_dot。
//      EchoEar 仓库没有这几张图，改用 LVGL 原生对象画（圆点、圆弧）；
//      图标位置留空给文本，不额外引资源。
//   2) 步数与每日任务在这台设备上暂时没有数据源，先按 Coze 的几何结构搭好，
//      显示占位值，等后续接上数据后调用 SetSteps() / SetMissionPercent()。
//
// 模拟表盘：原版表针是 PNG，这里改用 lv_line 直接画线条，功能等价、体积为零。
//
// 刷新策略：lv_timer 每 1000ms 在 LVGL 任务里刷新；离开页面时删除 timer，
//   保证桌面上没有后台刷屏。
// ============================================================================
#pragma once

#include "ui_page.h"

#include <cstdint>
#include <ctime>   // struct tm（ApplyAnalogHands 的参数）

namespace coze_ui {

class TimerPage : public UiPage {
public:
    UIBackend Backend() const override { return UIBackend::LVGL; }
    const char* Name() const override { return "Timer"; }

    void OnEnter() override;
    void OnExit() override;
    bool OnGesture(SwipeDir dir) override;

    // ---- 数据接口（供后续接入真实数据源）----
    // 步数：-1 表示"无数据"，显示 "--"（EchoEar 目前没有计步能力）。
    void SetSteps(int steps);
    // 每日任务完成度 0~100：-1 表示"无数据"，弧显示为 0 且文本 "--"。
    void SetMissionPercent(int percent);

private:
    void Build();
    void BuildDigital(lv_obj_t* parent);
    void BuildAnalog(lv_obj_t* parent);
    void Refresh();                       // 真正写文本/转指针（必须在 LVGL 任务里）
    void ApplyAnalogHands(const struct tm& t);
    void ApplySecDot(const struct tm& t); // 数字表盘的秒点
    void ShowFace(bool analog);           // 切换数字/模拟两屏
    static void TimerCb(lv_timer_t* timer);
    static void PressedCb(lv_event_t* e);
    static void PressLostCb(lv_event_t* e);
    static void ReleasedCb(lv_event_t* e);
    static void ClickedCb(lv_event_t* e);

    lv_obj_t* root_ = nullptr;

    // ---- 数字表盘 ----
    lv_obj_t* digital_ = nullptr;
    lv_obj_t* lbl_hour_ = nullptr;
    lv_obj_t* lbl_min_ = nullptr;
    lv_obj_t* lbl_year_ = nullptr;
    lv_obj_t* lbl_month_ = nullptr;
    lv_obj_t* lbl_weekday_ = nullptr;
    lv_obj_t* sec_dot_ = nullptr;           // 秒点（Coze 原图中它是一个静止的装饰点）

    // 电量组（77x22 面板 + 百分比文本，对应 Coze 的 battery_group + label）
    lv_obj_t* panel_battery_ = nullptr;
    lv_obj_t* lbl_battery_ = nullptr;

    // 步数组（100x26 面板 + 步数文本，对应 Coze 的 step_group + step_label）
    lv_obj_t* panel_steps_ = nullptr;
    lv_obj_t* lbl_steps_ = nullptr;
    int steps_ = -1;                      // -1 = 无数据

    // 每日任务组（283x185 + 330x330 arc + 百分比文本）
    lv_obj_t* panel_mission_ = nullptr;
    lv_obj_t* arc_mission_ = nullptr;
    lv_obj_t* lbl_mission_ = nullptr;
    int mission_percent_ = -1;            // -1 = 无数据

    // ---- 模拟表盘 ----
    lv_obj_t* analog_ = nullptr;
    lv_obj_t* hand_hour_ = nullptr;       // 三根指针都是 lv_line，点数组常驻
    lv_obj_t* hand_min_ = nullptr;
    lv_obj_t* hand_sec_ = nullptr;
    lv_point_precise_t pts_hour_[2] = {};
    lv_point_precise_t pts_min_[2] = {};
    lv_point_precise_t pts_sec_[2] = {};

    lv_timer_t* timer_ = nullptr;

    bool built_ = false;
    bool analog_shown_ = false;           // 当前显示的是模拟表盘还是数字表盘
    bool press_lost_ = false;   // 与 Coze 一致：拖出图标后松手不算点击
    bool refresh_pending_ = false;
};

}  // namespace coze_ui
