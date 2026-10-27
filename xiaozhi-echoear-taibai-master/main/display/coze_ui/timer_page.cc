// ============================================================================
// timer_page.cc — Timer 应用（移植自 esp-brookesia app_timer 的数字表盘）
//
// 布局与交互 1:1 对应 Coze 的 ui_Screen_watch_digital：
//   UI 参照 360x360 屏幕（与本机一致）。
//   秒/分/日期/电量四组文本，点击屏幕强制刷新一次。
// ============================================================================
#include "timer_page.h"

// Coze（ESP-Brookesia speaker release/v0.6）应用图标，逐字节移植自
// apps/brookesia_app_timer/ui/images/。它们是 .c 文件，用 extern "C" 引用。
extern "C" {
extern const lv_image_dsc_t ui_img_flash_png;
extern const lv_image_dsc_t ui_img_step_png;
extern const lv_image_dsc_t ui_img_daily_mission_png;
extern const lv_image_dsc_t ui_img_sec_dot_png;
}

#include "page_manager.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <cstdio>
#include <ctime>
#include <cmath>   // cosf / sinf：模拟表盘的指针角度换算

#define TAG "AppTimer"

namespace coze_ui {

namespace {

const char* const kMonthNames[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};
const char* const kWeekdayNames[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    // ★ label 默认带 LV_OBJ_FLAG_CLICKABLE。数字表盘上时分秒那几个大字号
    //   label 正好盖在屏幕中央，点击落在它们身上时就【停在那里】，不会冒泡到
    //   root_，于是 root_ 的 ClickedCb 永远收不到 —— 表现就是"点时钟没反应"。
    //   统一在这里清掉，所有 label 一律不参与命中测试。
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

}  // namespace

void TimerPage::Build()
{
    if (built_) {
        return;
    }

    root_ = lv_obj_create(lv_display_get_screen_active(lv_display_get_default()));
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    // 点击事件挂在 root_ 上（两屏都是它的子对象），点击 root 也能收到。
    lv_obj_add_event_cb(root_, PressedCb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(root_, PressLostCb, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(root_, ReleasedCb, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(root_, ClickedCb, LV_EVENT_CLICKED, this);

    BuildDigital(root_);
    BuildAnalog(root_);

    // 默认显示数字表盘（与 Coze 一致：run() 里先 loadScreen(digital)）
    analog_shown_ = false;
    if (analog_ != nullptr) {
        lv_obj_add_flag(analog_, LV_OBJ_FLAG_HIDDEN);
    }

    built_ = true;
    ESP_LOGW(TAG, "Build done");
}

void TimerPage::BuildDigital(lv_obj_t* parent)
{
    digital_ = lv_obj_create(parent);
    lv_obj_set_size(digital_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(digital_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(digital_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(digital_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(digital_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(digital_, LV_OBJ_FLAG_CLICKABLE);   // 点击交给 root_

    // ---- 主时钟：小时 + 分钟 ----
    // ★ 这两格的定位方式与下面日期三格【不同】，必须照 Coze 原样区分：
    //
    //   Coze 的 hour / minute 只调 lv_obj_set_x/y，【没有】set_align，
    //   所以 (100,120) 和 (210,140) 是相对父对象左上角的【绝对像素坐标】。
    //   而日期三格 + sec_dot 有 set_align(LV_ALIGN_CENTER)，
    //   它们的坐标是相对屏幕中心的【偏移量】。
    //
    //   这里用 lv_obj_set_pos() 给绝对坐标来复刻前者，用 lv_obj_align(...,
    //   LV_ALIGN_CENTER, dx, dy) 复刻后者。曾经把 hour/minute 也写成 CENTER
    //   对齐 + 偏移，结果整体偏出约半个标签宽 —— 因为字号经 transform_scale
    //   放大后宽度很大，对齐基准从"左上角"换成"中心"就差得非常多。
    //
    //   字号：hour 40px x 5.0 (500)，minute 40px x 4.0 (400)，与 Coze 一致。
    lbl_hour_ = MakeLabel(digital_, &lv_font_montserrat_40, 0xFFFFFF);
    lv_label_set_text(lbl_hour_, "00");
    lv_obj_set_pos(lbl_hour_, 100, 120);
    lv_obj_set_style_transform_scale(lbl_hour_, 500, LV_PART_MAIN);

    lbl_min_ = MakeLabel(digital_, &lv_font_montserrat_40, 0xEE1E1E);
    lv_label_set_text(lbl_min_, "00");
    lv_obj_set_pos(lbl_min_, 210, 140);
    lv_obj_set_style_transform_scale(lbl_min_, 400, LV_PART_MAIN);

    // ---- 顶部日期行：年 / 月日 / 星期（三格分离）----
    // Coze 三者的坐标分别是 CENTER+(90,-78) / CENTER+(20,-80) / CENTER+(-70,-80)，
    // 字号 20 / 24 / 30。三者都用 CENTER 对齐，靠 x 偏移排开。
    lbl_year_ = MakeLabel(digital_, &lv_font_montserrat_20, 0xFFFFFF);
    lv_label_set_text(lbl_year_, "----");
    lv_obj_align(lbl_year_, LV_ALIGN_CENTER, 90, -78);

    lbl_month_ = MakeLabel(digital_, &lv_font_montserrat_24, 0xFFFFFF);
    lv_label_set_text(lbl_month_, "---");
    lv_obj_align(lbl_month_, LV_ALIGN_CENTER, 20, -80);

    lbl_weekday_ = MakeLabel(digital_, &lv_font_montserrat_30, 0xFFFFFF);
    lv_label_set_text(lbl_weekday_, "---");
    lv_obj_align(lbl_weekday_, LV_ALIGN_CENTER, -70, -80);

    // ---- 电量组：77x22 面板 + 百分比 ----
    // Coze 的 Panel_battery_group2：77x22，TOP_MID+(0,20)，
    // flex ROW / 居中，bg_opa 0（透底）、border_width 0、pad_row 6、pad_column 8。
    // 原版这组里还有一个 flash 图标，EchoEar 没有该 PNG，省略图标只留文本，
    // 面板本身保留，后续要加图标直接 lv_image_create(panel_battery_) 塞进去即可。
    panel_battery_ = lv_obj_create(digital_);
    lv_obj_set_size(panel_battery_, 77, 22);
    lv_obj_align(panel_battery_, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_flex_flow(panel_battery_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel_battery_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(panel_battery_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel_battery_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(panel_battery_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_battery_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel_battery_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel_battery_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel_battery_, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(panel_battery_, 8, LV_PART_MAIN);

    // Coze 的 Panel_battery_group2 里是 [flash 图标, "86%" 文本]，flex ROW 居中。
    // 顺寉必须一致：图标先创建，才会排在文字左边。
    // Coze 原图 ui_img_flash_png（13x18），与 SquareLine 工程逐字节一致。
    lv_obj_t* flash_icon = lv_image_create(panel_battery_);
    lv_image_set_src(flash_icon, &ui_img_flash_png);
    lv_obj_remove_flag(flash_icon, LV_OBJ_FLAG_CLICKABLE);   // 点击交给 root_
    lbl_battery_ = MakeLabel(panel_battery_, &lv_font_montserrat_24, 0xFFFFFF);
    lv_label_set_text(lbl_battery_, "--");

    // ---- 步数组：100x26 面板 + 步数文本 ----
    // Coze 的 Panel_step_group1：100x26，BOTTOM_MID+(0,-40)，
    // flex ROW / 主轴上 END 对齐（靠右），pad_row 0、pad_column 8。
    // 原版里还有一个 step 图标，同样没有对应 PNG，省略。
    panel_steps_ = lv_obj_create(digital_);
    lv_obj_set_size(panel_steps_, 100, 26);
    lv_obj_align(panel_steps_, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_flex_flow(panel_steps_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel_steps_, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(panel_steps_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel_steps_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(panel_steps_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_steps_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel_steps_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel_steps_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel_steps_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(panel_steps_, 8, LV_PART_MAIN);

    // Coze 的 Panel_step_group1 里是 [step 图标, "1526" 文本]，flex ROW 靠右。
    // Coze 原图 ui_img_step_png（19x15）。
    lv_obj_t* step_icon = lv_image_create(panel_steps_);
    lv_image_set_src(step_icon, &ui_img_step_png);
    lv_obj_remove_flag(step_icon, LV_OBJ_FLAG_CLICKABLE);    // 点击交给 root_
    lbl_steps_ = MakeLabel(panel_steps_, &lv_font_montserrat_20, 0xFFFFFF);
    lv_label_set_text(lbl_steps_, "--");
    // Coze 的 Label_step_label1 额外调了 lv_obj_set_x(2) / set_y(-4) 且
    // 带 LV_ALIGN_CENTER。它是 flex 子项，align 与 flex 布局会互相覆盖，
    // 实际效果是把文字在单元格内轻微上移右移 2~4px。这里复刻这个微调。
    lv_obj_align(lbl_steps_, LV_ALIGN_CENTER, 2, -4);

    // ---- 每日任务组：283x185 面板 + 330x330 弧 + 百分比 ----
    // Coze 的 Panel_daily_mission_group2：283x185，BOTTOM_LEFT（贴左下角），
    // 不可点击、不可滚动，bg 透明、border 0。
    //   弧 Arc_daily_mission_arc1：330x330，CENTER+(33,-87)，值 87，
    //      bg_angles 88..170，MAIN 描边 0x303030 宽 5 圆头，
    //      INDICATOR 描边 0xB0E14A 宽 5 圆头。
    //   百分比文本：BOTTOM_LEFT+(85,-70)，0xB0E14A，montserrat_20。
    //   每日任务图标（Image_daily_mission_image1，旋转 -120 度）使用 Coze 原图。
    panel_mission_ = lv_obj_create(digital_);
    lv_obj_set_size(panel_mission_, 283, 185);
    lv_obj_align(panel_mission_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_remove_flag(panel_mission_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel_mission_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(panel_mission_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_mission_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel_mission_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel_mission_, 0, LV_PART_MAIN);

    arc_mission_ = lv_arc_create(panel_mission_);
    lv_obj_set_size(arc_mission_, 330, 330);
    lv_obj_align(arc_mission_, LV_ALIGN_CENTER, 33, -87);
    lv_obj_remove_flag(arc_mission_, LV_OBJ_FLAG_CLICKABLE);
    // 弧不做交互，去掉旋钮（否则会有一个白色小点乱跑）
    lv_obj_remove_style(arc_mission_, NULL, LV_PART_KNOB);
    lv_arc_set_range(arc_mission_, 0, 100);
    lv_arc_set_value(arc_mission_, 0);
    lv_arc_set_bg_angles(arc_mission_, 88, 170);
    lv_obj_set_style_arc_color(arc_mission_, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_mission_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_mission_, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc_mission_, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_mission_, lv_color_hex(0xB0E14A), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc_mission_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_mission_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_mission_, true, LV_PART_INDICATOR);

    lbl_mission_ = MakeLabel(panel_mission_, &lv_font_montserrat_20, 0xB0E14A);
    lv_label_set_text(lbl_mission_, "--");
    lv_obj_align(lbl_mission_, LV_ALIGN_BOTTOM_LEFT, 85, -70);

    // Coze 的 Image_daily_mission_image1：CENTER+(-57,16)，用 lv_image_set_rotation(-120)。
    // 使用 Coze 原图 ui_img_daily_mission_png（84x114），不再用手绘方块近似。
    //
    // ★ 关键：lv_image_set_rotation 的参数单位是【0.1°】（lv_image.h:121 注释
    //   "rotation in degree with 0.1 degree resolution (0..3600)"），
    //   实现里 draw 时 `rotation / 10.0f`（vg_lite/nema_gfx 等后端一致）。
    //   所以 Coze 写的 -120 实际是 -12°。这里严格照搬 Coze 的原值 -120，
    //   不要"自作聪明"改成 -1200 —— 那样才是真的 -120°。
    //
    //   之前误用样式变换 lv_obj_set_style_transform_rotation 的问题在于：
    //   样式变换的 pivot 参考系与 lv_image_set_rotation 不一致，
    //   导致字符朝向表盘中心（时间）而不是顺着弧线。
    //   现在只用 lv_image_set_rotation，绕图片中心（默认 pivot W/2,H/2）旋转。
    lv_obj_t* mission_icon = lv_image_create(panel_mission_);
    lv_image_set_src(mission_icon, &ui_img_daily_mission_png);
    lv_obj_align(mission_icon, LV_ALIGN_CENTER, -57, 16);
    lv_obj_remove_flag(mission_icon, LV_OBJ_FLAG_CLICKABLE); // 点击交给 root_
    lv_image_set_rotation(mission_icon, -120);

    // ---- 秒点 sec_dot ----
    // Coze 的 Image_sec_dot：8x8 图片，CENTER+(10,15)，pivot(4,194)。
    //
    // ★ 重要更正：这个点在 Coze 里【完全不移动】。
    //   esp_brookesia_app_timer.cpp 的 updateTimeDisplay() / updateAnalogClock()
    //   都没有操作它，它只在 ui_Screen_watch_digital_screen_init() 里设一次 pivot
    //   就固定在 (10,15)。而 LVGL 的 pivot 只有在该对象被【旋转或缩放】时才参与
    //   变换，纯平移的图 pivot 不起任何作用 —— 所以它在 Coze 里就是个不动的小白点。
    //
    //   我之前把它理解成"绕表盘公转的秒点"，在 Refresh() 里每秒重算坐标，
    //   结果屏幕上出现"一个点在平移"的怪象。现在改回：固定在 (10,15)，不动。
    sec_dot_ = lv_image_create(digital_);
    lv_image_set_src(sec_dot_, &ui_img_sec_dot_png);
    lv_obj_align(sec_dot_, LV_ALIGN_CENTER, 10, 15);
    lv_obj_remove_flag(sec_dot_, LV_OBJ_FLAG_CLICKABLE);    // 点击交给 root_
}

// ----------------------------------------------------------------------------
// 模拟表盘：全部用 LVGL 原生对象画，不依赖任何图片资源。
//
// 为什么不用 Coze 的 ui_watch_analog_Image_hour/min/sec：
//   那三张表针是 PNG，EchoEar 仓库里没有对应资源文件，
//   而 lv_line 画一条带圆头的线段视觉上等价，体积为零，也不需要解码。
//
// 坐标系：root_ 是全屏 360x360，中心在 (180,180)。
//   lv_line 的坐标是"相对自身对象"的，所以每个指针对象都定位在左上角 (0,0)
//   且尺寸铺满全屏，点坐标就可以直接用屏幕绝对坐标，省掉所有偏移换算。
// ----------------------------------------------------------------------------
void TimerPage::BuildAnalog(lv_obj_t* parent)
{
    constexpr int kCenterX = 180;
    constexpr int kCenterY = 180;
    constexpr int kHourLen = 70;
    constexpr int kMinLen = 100;
    constexpr int kSecLen = 112;

    analog_ = lv_obj_create(parent);
    lv_obj_set_size(analog_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(analog_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(analog_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(analog_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(analog_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(analog_, LV_OBJ_FLAG_CLICKABLE);

    // ---- 12 格刻度 ----
    // 每个刻度是一条从半径 r1 到 r2 的短线，用 lv_line 画。
    // lv_line 的点数组必须在对象生命周期内有效，所以用 static 常驻。
    static lv_point_precise_t tick_pts[12][2];
    static lv_obj_t* tick_objs[12] = {};
    constexpr int kTickOuter = 150;
    constexpr int kTickInnerLong = 132;
    constexpr int kTickInnerShort = 140;
    for (int i = 0; i < 12; i++) {
        // 从 12 点方向开始，顺时针每 30 度一格
        const float angle = (static_cast<float>(i) * 30.0f - 90.0f) * 3.14159265f / 180.0f;
        const int inner = (i % 3 == 0) ? kTickInnerLong : kTickInnerShort;
        const float ca = cosf(angle);
        const float sa = sinf(angle);
        tick_pts[i][0].x = static_cast<lv_value_precise_t>(kCenterX + ca * inner);
        tick_pts[i][0].y = static_cast<lv_value_precise_t>(kCenterY + sa * inner);
        tick_pts[i][1].x = static_cast<lv_value_precise_t>(kCenterX + ca * kTickOuter);
        tick_pts[i][1].y = static_cast<lv_value_precise_t>(kCenterY + sa * kTickOuter);

        lv_obj_t* tick = lv_line_create(analog_);
    lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(tick, 0, 0);
        lv_obj_set_size(tick, LV_PCT(100), LV_PCT(100));
        lv_line_set_points(tick, tick_pts[i], 2);
        lv_obj_set_style_line_width(tick, (i % 3 == 0) ? 4 : 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(tick, lv_color_hex((i % 3 == 0) ? 0xFFFFFF : 0x808080),
                                    LV_PART_MAIN);
        lv_obj_set_style_line_rounded(tick, true, LV_PART_MAIN);
        tick_objs[i] = tick;
    }

    // ---- 三根指针 ----
    // 初始都指向 12 点，具体角度由 ApplyAnalogHands() 每秒更新。
    {
        const float ca = cosf(-3.14159265f / 2.0f);   // -90 度 = 12 点方向
        const float sa = sinf(-3.14159265f / 2.0f);
        pts_hour_[0].x = kCenterX;
        pts_hour_[0].y = kCenterY;
        pts_hour_[1].x = static_cast<lv_value_precise_t>(kCenterX + ca * kHourLen);
        pts_hour_[1].y = static_cast<lv_value_precise_t>(kCenterY + sa * kHourLen);

        pts_min_[0].x = kCenterX;
        pts_min_[0].y = kCenterY;
        pts_min_[1].x = static_cast<lv_value_precise_t>(kCenterX + ca * kMinLen);
        pts_min_[1].y = static_cast<lv_value_precise_t>(kCenterY + sa * kMinLen);

        pts_sec_[0].x = kCenterX;
        pts_sec_[0].y = kCenterY;
        pts_sec_[1].x = static_cast<lv_value_precise_t>(kCenterX + ca * kSecLen);
        pts_sec_[1].y = static_cast<lv_value_precise_t>(kCenterY + sa * kSecLen);
    }

    hand_hour_ = lv_line_create(analog_);
    lv_obj_remove_flag(hand_hour_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(hand_hour_, 0, 0);
    lv_obj_set_size(hand_hour_, LV_PCT(100), LV_PCT(100));
    lv_line_set_points(hand_hour_, pts_hour_, 2);
    lv_obj_set_style_line_width(hand_hour_, 8, LV_PART_MAIN);
    lv_obj_set_style_line_color(hand_hour_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(hand_hour_, true, LV_PART_MAIN);

    hand_min_ = lv_line_create(analog_);
    lv_obj_remove_flag(hand_min_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(hand_min_, 0, 0);
    lv_obj_set_size(hand_min_, LV_PCT(100), LV_PCT(100));
    lv_line_set_points(hand_min_, pts_min_, 2);
    lv_obj_set_style_line_width(hand_min_, 5, LV_PART_MAIN);
    lv_obj_set_style_line_color(hand_min_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(hand_min_, true, LV_PART_MAIN);

    hand_sec_ = lv_line_create(analog_);
    lv_obj_remove_flag(hand_sec_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(hand_sec_, 0, 0);
    lv_obj_set_size(hand_sec_, LV_PCT(100), LV_PCT(100));
    lv_line_set_points(hand_sec_, pts_sec_, 2);
    lv_obj_set_style_line_width(hand_sec_, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(hand_sec_, lv_color_hex(0xEE1E1E), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(hand_sec_, true, LV_PART_MAIN);

    // ---- 中心圆点，盖住三针交点 ----
    lv_obj_t* hub = lv_obj_create(analog_);
    lv_obj_set_size(hub, 14, 14);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0xEE1E1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hub, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);
}

// 按当前时刻把三根指针转到正确角度。
// 角度换算与 Coze 一致：均从 12 点方向顺时针。
void TimerPage::ApplyAnalogHands(const struct tm& t)
{
    if (hand_hour_ == nullptr || hand_min_ == nullptr || hand_sec_ == nullptr) {
        return;
    }
    constexpr int kCenterX = 180;
    constexpr int kCenterY = 180;
    constexpr int kHourLen = 70;
    constexpr int kMinLen = 100;
    constexpr int kSecLen = 112;

    // 时针要把分钟也算进去，否则整点之间时针是跳变的
    const float hour_deg = (static_cast<float>(t.tm_hour % 12) + t.tm_min / 60.0f) * 30.0f;
    const float min_deg = static_cast<float>(t.tm_min) * 6.0f;
    const float sec_deg = static_cast<float>(t.tm_sec) * 6.0f;

    auto set_hand = [](lv_point_precise_t* pts, float deg, int len) {
        const float rad = (deg - 90.0f) * 3.14159265f / 180.0f;
        pts[1].x = static_cast<lv_value_precise_t>(kCenterX + cosf(rad) * len);
        pts[1].y = static_cast<lv_value_precise_t>(kCenterY + sinf(rad) * len);
    };
    set_hand(pts_hour_, hour_deg, kHourLen);
    set_hand(pts_min_, min_deg, kMinLen);
    set_hand(pts_sec_, sec_deg, kSecLen);

    // 修改点坐标后必须重新 set_points 才会触发重绘
    lv_line_set_points(hand_hour_, pts_hour_, 2);
    lv_line_set_points(hand_min_, pts_min_, 2);
    lv_line_set_points(hand_sec_, pts_sec_, 2);
}

// ----------------------------------------------------------------------------
// ApplySecDot —— 已停用，保留空实现以说明原因。
//
// 查 Coze 的 esp_brookesia_app_timer.cpp 后确认：
//   updateTimeDisplay() 和 updateAnalogClock() 都【没有】操作 sec_dot，
//   它只在 ui_Screen_watch_digital_screen_init() 里 lv_image_set_pivot(4,194)
//   设一次，然后就固定在那个位置、永不移动。
//
//   pivot 在 LVGL 里只在该对象被旋转/缩放（lv_image_set_rotation /
//   lv_image_set_scale）时才参与变换；纯平移的图片 pivot 没有任何效果。
//   所以 Coze 那个 pivot(4,194) 在数字表盘上是不产生视觉作用的。
//
// 我之前把它当成"绕表盘公转的秒点"，每秒重算坐标，导致屏幕上出现
// "一个点在平移"。现在 sec_dot 固定在 CENTER+(10,15)，本函数不再被调用。
// ----------------------------------------------------------------------------
void TimerPage::ApplySecDot(const struct tm& t)
{
    (void)t;
    // 无操作：sec_dot 是静止的装饰点，与 Coze 原版一致。
}
// 步数数据接口。EchoEar 没有计步能力，调用方拿到真实数据前
// 保持 steps_ = -1，界面显示 "--"。
void TimerPage::SetSteps(int steps)
{
    steps_ = (steps < 0) ? -1 : steps;
    // 只在界面已经搭好时立即刷新；没搭好就等 Build 后的第一帧 Refresh 读这个值。
    if (built_) {
        Refresh();
    }
}

// 每日任务完成度接口（0~100）。-1 表示无数据。
void TimerPage::SetMissionPercent(int percent)
{
    mission_percent_ = (percent < 0) ? -1 : (percent > 100 ? 100 : percent);
    if (built_) {
        Refresh();
    }
}

// 切换数字/模拟两屏。Coze 原版是 switchScreen()，逻辑一致。
void TimerPage::ShowFace(bool analog){
    if (digital_ == nullptr || analog_ == nullptr) {
        return;
    }
    analog_shown_ = analog;
    if (analog) {
        lv_obj_add_flag(digital_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(analog_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(analog_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(digital_, LV_OBJ_FLAG_HIDDEN);
    }
    // 切换后立即刷新一次，避免模拟屏出现"停在 12 点"的一帧
    Refresh();
}

void TimerPage::OnEnter()
{
    Build();

    if (root_ != nullptr) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
    }

    // 进入页面立刻刷新一次，避免首帧停在初始的 "00"
    Refresh();

    if (timer_ == nullptr) {
        timer_ = lv_timer_create(TimerCb, 1000, this);
    }

    // 诊断：进入页面后把关键状态全部打出来。之前"点 Timer 后看不到时钟"时
    // 串口里只有 OnEnter，看不出 root_ 是不是真的可见、有没有被别的对象盖住、
    // 数字屏是不是被 HIDDEN 掉了 —— 所以这里一次性把事实摊开。
    if (root_ != nullptr) {
        ESP_LOGW(TAG, "OnEnter: root visible=%d hidden=%d digital_hidden=%d "
                      "analog_hidden=%d face=%s built=%d",
                 (int)lv_obj_is_visible(root_),
                 (int)lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN),
                 digital_ ? (int)lv_obj_has_flag(digital_, LV_OBJ_FLAG_HIDDEN) : -1,
                 analog_ ? (int)lv_obj_has_flag(analog_, LV_OBJ_FLAG_HIDDEN) : -1,
                 analog_shown_ ? "analog" : "digital",
                 (int)built_);
        // 置顶之后再确认一次：如果 root_ 仍不是顶层屏幕的最后一个子对象，
        // 说明有别的页面把它盖住了。
        lv_obj_t* scr = lv_obj_get_parent(root_);
        if (scr != nullptr) {
            const int32_t cnt = lv_obj_get_child_count(scr);
            ESP_LOGW(TAG, "OnEnter: screen children=%d, root is last=%d",
                     (int)cnt, (int)(cnt > 0 && lv_obj_get_child(scr, cnt - 1) == root_));
        }
    } else {
        ESP_LOGE(TAG, "OnEnter: root_ is null!");
    }

    ESP_LOGW(TAG, "OnEnter done");
}

void TimerPage::OnExit()
{
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGW(TAG, "OnExit");
}

bool TimerPage::OnGesture(SwipeDir dir)
{
    // 不接管滑动：右滑回桌面由 PageManager 统一处理。
    (void)dir;
    return false;
}

void TimerPage::Refresh()
{
    if (root_ == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);

    char buf[32];

    if (lbl_hour_ != nullptr) {
        snprintf(buf, sizeof(buf), "%02d", tm_now.tm_hour);
        lv_label_set_text(lbl_hour_, buf);
    }
    if (lbl_min_ != nullptr) {
        snprintf(buf, sizeof(buf), "%02d", tm_now.tm_min);
        lv_label_set_text(lbl_min_, buf);
    }
    if (lbl_year_ != nullptr) {
        snprintf(buf, sizeof(buf), "%d", tm_now.tm_year + 1900);
        lv_label_set_text(lbl_year_, buf);
    }
    if (lbl_month_ != nullptr) {
        const int m = tm_now.tm_mon;
        const char* name = (m >= 0 && m < 12) ? kMonthNames[m] : "---";
        // Coze 原文是 "%d  %s"（日与月份之间【两个空格】），不是一格。
        // 这是 SquareLine 里 "18 FEB" 实际渲染出的间距，照搬。
        snprintf(buf, sizeof(buf), "%d  %s", tm_now.tm_mday, name);
        lv_label_set_text(lbl_month_, buf);
    }
    if (lbl_weekday_ != nullptr) {
        const int w = tm_now.tm_wday;
        lv_label_set_text(lbl_weekday_, (w >= 0 && w < 7) ? kWeekdayNames[w] : "---");
    }
    // 电量与表情页同源（EmoteDisplay 内部有 5s 节流），读不到就显示 "--"
    if (lbl_battery_ != nullptr) {
        const int pct = PageManager::GetInstance().BatteryPercent();
        if (pct < 0) {
            lv_label_set_text(lbl_battery_, "--");
        } else {
            snprintf(buf, sizeof(buf), "%d%%", pct);
            lv_label_set_text(lbl_battery_, buf);
        }
    }

    // 步数：没有数据源时显示 "--"（Coze 原版这里是固定值 1526 的占位）
    if (lbl_steps_ != nullptr) {
        if (steps_ < 0) {
            lv_label_set_text(lbl_steps_, "--");
        } else {
            snprintf(buf, sizeof(buf), "%d", steps_);
            lv_label_set_text(lbl_steps_, buf);
        }
    }

    // 每日任务：同样的处理。弧值直接驱动 INDICATOR 的长度，
    // Coze 里是靠 arc 的 VALUE_CHANGED 事件回填文本，这里自己算更直接。
    if (lbl_mission_ != nullptr) {
        if (mission_percent_ < 0) {
            lv_label_set_text(lbl_mission_, "--");
            if (arc_mission_ != nullptr) {
                lv_arc_set_value(arc_mission_, 0);
            }
        } else {
            snprintf(buf, sizeof(buf), "%d%%", mission_percent_);
            lv_label_set_text(lbl_mission_, buf);
            if (arc_mission_ != nullptr) {
                lv_arc_set_value(arc_mission_, mission_percent_);
            }
        }
    }

    // 数字表盘的秒点：每秒绕表盘一圈
    // （sec_dot 不再随秒移动：Coze 原版就是固定不动的，见 BuildDigital 注释）

    // 模拟表盘：即使当前显示的是数字屏也照常更新指针，
    // 这样切过去时不会出现"停在 12 点"的一帧。
    ApplyAnalogHands(tm_now);
}

void TimerPage::TimerCb(lv_timer_t* timer)
{
    auto* self = static_cast<TimerPage*>(lv_timer_get_user_data(timer));
    if (self == nullptr) {
        return;
    }
    // lv_timer 回调本身就跑在 LVGL 任务里，直接刷新即可。
    if (self->refresh_pending_) {
        return;
    }
    self->refresh_pending_ = true;
    self->Refresh();
    self->refresh_pending_ = false;
}

// ----------------------------------------------------------------------------
// 点击处理：只有"按下后没有丢失"才算点击，防止滑动时误触发。
// ----------------------------------------------------------------------------
void TimerPage::PressedCb(lv_event_t* e)
{
    auto* self = static_cast<TimerPage*>(e != nullptr ? lv_event_get_user_data(e) : nullptr);
    if (self != nullptr) {
        self->press_lost_ = false;
        // 用 WARN 级别：本工程 CONFIG_LOG_DEFAULT_LEVEL_WARN，
        // ESP_LOGD 在串口里根本看不到，调试期一律用 W。
        ESP_LOGW(TAG, "PRESS on clock (face=%s)", self->analog_shown_ ? "analog" : "digital");
    }
}

void TimerPage::PressLostCb(lv_event_t* e)
{
    auto* self = static_cast<TimerPage*>(e != nullptr ? lv_event_get_user_data(e) : nullptr);
    if (self != nullptr) {
        self->press_lost_ = true;
        ESP_LOGW(TAG, "PRESS_LOST (slid off the widget)");
    }
}

void TimerPage::ReleasedCb(lv_event_t* e)
{
    (void)e;
}

void TimerPage::ClickedCb(lv_event_t* e)
{
    auto* self = static_cast<TimerPage*>(e != nullptr ? lv_event_get_user_data(e) : nullptr);
    if (self == nullptr) {
        return;
    }
    ESP_LOGW(TAG, "CLICKED press_lost=%d -> switching face", (int)self->press_lost_);
    if (self->press_lost_) {
        self->press_lost_ = false;
        return;
    }
    // 与 Coze 的 switchScreen() 一致：点一下数字表盘切到模拟表盘，再点切回来。
    self->ShowFace(!self->analog_shown_);
    ESP_LOGW(TAG, "face now = %s", self->analog_shown_ ? "analog" : "digital");
}

}  // namespace coze_ui
