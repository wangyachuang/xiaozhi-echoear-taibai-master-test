// ============================================================================
// clock_page_lvgl.cc — 时钟 App（复刻 Coze Timer 风格）
// ----------------------------------------------------------------------------
// 视觉规格（参考 esp-brookesia apps/brookesia_app_timer 的 SquareLine 工程）：
//   背景      #000000
//   大号时间  BUILTIN_TEXT_FONT + transform 2.5x（官方是 100px Number_big）
//   时间颜色  时=青 #00C8FF  分=红 #FF4D5E  冒号=青
//   日期      小字，左侧
//   电池      分档图标 + "NN%"，右上
//   面板      深灰蓝 #1A2430 圆角卡片
//   点击屏幕  数字 <-> 模拟
//
// 性能：
//   * 1Hz 定时器只在页面可见时运行
//   * 数字页只在分钟变化时改文本；模拟页只在秒变化时重画指针
// ============================================================================
#include "clock_page_lvgl.h"

#if CONFIG_EMOTE_AS_LVGL_APP

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "board.h"        // Board::GetInstance().GetBatteryLevel()
#include "battery_icon.h" // 电池分档图标

#define TAG "ClockPage"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace clock_page_lvgl {

namespace {

constexpr int kScreenW = 360;
constexpr int kScreenH = 360;

// 配色（对齐官方 Smartwatch 工程的深色主题）
constexpr uint32_t kBgColor = 0x000000;
constexpr uint32_t kCardBg = 0x1A2430;
constexpr uint32_t kCardBorder = 0x2C3A4A;
constexpr uint32_t kAccent = 0x00C8FF;     // 小时/主色
constexpr uint32_t kAccentRed = 0xFF4D5E;  // 分钟
constexpr uint32_t kDim = 0x5A6A7A;
constexpr uint32_t kBright = 0xFFFFFF;
constexpr uint32_t kGreen = 0x2ECC71;

// 大号时间的放大倍数（256 = 1.0x）。BUILTIN_TEXT_FONT 是 20px，
// 放大 3 倍得到 60px，接近官方 100px 但更省 CPU（整屏重绘代价小）。
constexpr int32_t kTimeScale = 768;   // 3.0x

enum class Face : uint8_t { DIGITAL = 0, ANALOG = 1 };

struct State {
    lv_obj_t* page = nullptr;

    // 数字页
    lv_obj_t* digital_root = nullptr;
    lv_obj_t* label_hh = nullptr;
    lv_obj_t* label_mm = nullptr;
    lv_obj_t* label_colon = nullptr;
    lv_obj_t* label_ampm = nullptr;

    // 模拟页
    lv_obj_t* analog_root = nullptr;
    lv_obj_t* dial = nullptr;
    lv_obj_t* hand_hour = nullptr;
    lv_obj_t* hand_min = nullptr;
    lv_obj_t* hand_sec = nullptr;
    lv_point_precise_t pts_hour[2] = {{0, 0}, {0, 0}};
    lv_point_precise_t pts_min[2] = {{0, 0}, {0, 0}};
    lv_point_precise_t pts_sec[2] = {{0, 0}, {0, 0}};

    // 共用
    lv_obj_t* label_date = nullptr;
    lv_obj_t* label_battery = nullptr;
    lv_obj_t* img_battery = nullptr;
    lv_obj_t* label_hint = nullptr;

    lv_timer_t* timer = nullptr;
    Face face = Face::DIGITAL;

    int last_hh = -1;
    int last_mm = -1;
    int last_sec = -1;
    int last_battery = -1;
    int last_charging = -1;
    int last_yday = -1;
} s;

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// 圆角卡片容器
lv_obj_t* MakeCard(lv_obj_t* parent, int w, int h)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(kCardBorder), 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

void ReadBattery(int& level, bool& charging)
{
    level = -1;
    charging = false;
    int lv = -1;
    bool chg = false;
    bool dis = false;
    if (Board::GetInstance().GetBatteryLevel(lv, chg, dis) && lv >= 0) {
        level = lv;
        charging = chg;
    }
}

void PointAt(lv_point_precise_t* pt, float deg, float len, float cx, float cy)
{
    const float rad = (deg - 90.0f) * 3.14159265f / 180.0f;
    pt->x = static_cast<lv_value_precise_t>(cx + len * cosf(rad));
    pt->y = static_cast<lv_value_precise_t>(cy + len * sinf(rad));
}

void UpdateCommonText()
{
    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    const bool time_valid = tm_now.tm_year >= 2025 - 1900;

    // 日期：2025-07-18 周五
    if (time_valid && tm_now.tm_yday != s.last_yday) {
        s.last_yday = tm_now.tm_yday;
        static const char* kWday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        char buf[48];
        // 全部用无符号 + 宽度精度，输出长度可被编译器证明：
        //   %u 年(4位) - %.2u 月 - %.2u 日 + %.3s 星期
        snprintf(buf, sizeof(buf), "%u-%.2u-%.2u  %.3s",
                 static_cast<unsigned>(tm_now.tm_year + 1900),
                 static_cast<unsigned>(tm_now.tm_mon + 1),
                 static_cast<unsigned>(tm_now.tm_mday),
                 kWday[tm_now.tm_wday % 7]);
        lv_label_set_text(s.label_date, buf);
    }

    int level = -1;
    bool charging = false;
    ReadBattery(level, charging);
    if (level != s.last_battery || static_cast<int>(charging) != s.last_charging) {
        s.last_battery = level;
        s.last_charging = charging ? 1 : 0;
        if (level < 0) {
            lv_label_set_text(s.label_battery, "--");
            lv_obj_add_flag(s.img_battery, LV_OBJ_FLAG_HIDDEN);
        } else {
            // 回收成 uint8_t，让 -Wformat-truncation 能证明输出上限
            const uint8_t pct = static_cast<uint8_t>(level > 100 ? 100 : level);
            char buf[16];
            snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(pct));
            lv_label_set_text(s.label_battery, buf);
            lv_obj_remove_flag(s.img_battery, LV_OBJ_FLAG_HIDDEN);
            if (const lv_image_dsc_t* d = battery_icon::ImageFor(charging, level)) {
                lv_image_set_src(s.img_battery, d);
            }
        }
    }
}

void RedrawHands(const struct tm& t)
{
    const float cx = 180.0f;
    const float cy = 180.0f;
    const float sec_deg = static_cast<float>(t.tm_sec) * 6.0f;
    const float min_deg = static_cast<float>(t.tm_min) * 6.0f + static_cast<float>(t.tm_sec) * 0.1f;
    const float hour_deg = (static_cast<float>(t.tm_hour % 12) + static_cast<float>(t.tm_min) / 60.0f) * 30.0f;

    PointAt(&s.pts_hour[0], hour_deg, -18.0f, cx, cy);
    PointAt(&s.pts_hour[1], hour_deg, 78.0f, cx, cy);
    PointAt(&s.pts_min[0], min_deg, -26.0f, cx, cy);
    PointAt(&s.pts_min[1], min_deg, 116.0f, cx, cy);
    PointAt(&s.pts_sec[0], sec_deg, -34.0f, cx, cy);
    PointAt(&s.pts_sec[1], sec_deg, 128.0f, cx, cy);

    lv_obj_invalidate(s.hand_hour);
    lv_obj_invalidate(s.hand_min);
    lv_obj_invalidate(s.hand_sec);
}

void ApplyFace()
{
    const bool digital = (s.face == Face::DIGITAL);
    if (digital) {
        lv_obj_remove_flag(s.digital_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.analog_root, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s.label_hint, "tap to switch analog");
    } else {
        lv_obj_add_flag(s.digital_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s.analog_root, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s.label_hint, "tap to switch digital");
        const time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        if (tm_now.tm_year >= 2025 - 1900) {
            RedrawHands(tm_now);
        }
    }
}

void OnDialClicked(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s.face = (s.face == Face::DIGITAL) ? Face::ANALOG : Face::DIGITAL;
    ApplyFace();
    UpdateCommonText();
}

// ---------------------------------------------------------------------------
// 数字页构建
// ---------------------------------------------------------------------------
void BuildDigital()
{
    s.digital_root = lv_obj_create(s.page);
    lv_obj_remove_style_all(s.digital_root);
    lv_obj_set_size(s.digital_root, kScreenW, kScreenH);
    lv_obj_center(s.digital_root);
    lv_obj_remove_flag(s.digital_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.digital_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s.digital_root, OnDialClicked, LV_EVENT_CLICKED, nullptr);

    // ---- 大号 HH:MM ----
    lv_obj_t* row = lv_obj_create(s.digital_root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto make_time_part = [&](uint32_t color) {
        lv_obj_t* l = MakeLabel(row, &BUILTIN_TEXT_FONT, color);
        lv_label_set_text(l, "--");
        lv_obj_set_style_transform_scale(l, kTimeScale, 0);
        lv_obj_set_style_transform_pivot_x(l, 0, 0);
        lv_obj_set_style_transform_pivot_y(l, 0, 0);
        return l;
    };
    s.label_hh = make_time_part(kAccent);
    s.label_colon = make_time_part(kAccent);
    lv_label_set_text(s.label_colon, ":");
    s.label_mm = make_time_part(kAccentRed);

    // AM/PM 小标
    s.label_ampm = MakeLabel(row, &lv_font_montserrat_14, kDim);
    lv_label_set_text(s.label_ampm, "");
    lv_obj_set_style_pad_left(s.label_ampm, 8, 0);

    // ---- 底部信息卡：日期 + 电池 ----
    lv_obj_t* card = MakeCard(s.digital_root, 220, 52);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -46);

    s.label_date = MakeLabel(card, &lv_font_montserrat_14, kDim);
    lv_label_set_text(s.label_date, "");
    lv_obj_align(s.label_date, LV_ALIGN_LEFT_MID, 14, 0);

    s.img_battery = lv_image_create(card);
    lv_image_set_src(s.img_battery, battery_icon::ImageFor(false, 100));
    lv_obj_set_size(s.img_battery, 26, 26);
    lv_obj_align(s.img_battery, LV_ALIGN_RIGHT_MID, -52, 0);
    lv_obj_add_flag(s.img_battery, LV_OBJ_FLAG_HIDDEN);

    s.label_battery = MakeLabel(card, &lv_font_montserrat_14, kGreen);
    lv_label_set_text(s.label_battery, "--");
    lv_obj_align(s.label_battery, LV_ALIGN_RIGHT_MID, -14, 0);
}

// ---------------------------------------------------------------------------
// 模拟页构建
// ---------------------------------------------------------------------------
void BuildDial()
{
    s.analog_root = lv_obj_create(s.page);
    lv_obj_remove_style_all(s.analog_root);
    lv_obj_set_size(s.analog_root, kScreenW, kScreenH);
    lv_obj_center(s.analog_root);
    lv_obj_remove_flag(s.analog_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.analog_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s.analog_root, OnDialClicked, LV_EVENT_CLICKED, nullptr);

    s.dial = lv_obj_create(s.analog_root);
    lv_obj_remove_style_all(s.dial);
    lv_obj_set_size(s.dial, 280, 280);
    lv_obj_center(s.dial);
    lv_obj_remove_flag(s.dial, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s.dial, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_radius(s.dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s.dial, 2, 0);
    lv_obj_set_style_border_color(s.dial, lv_color_hex(kCardBorder), 0);
    lv_obj_set_style_bg_opa(s.dial, LV_OPA_TRANSP, 0);

    // 刻度（用 dial 的局部坐标 0..280，中心 140）
    // lv_line_set_points 只保存指针不拷贝，所以每个刻度必须有自己
    // 的静态存储；共用一个数组会让所有刻度画在同一位置。
    const float cx = 140.0f;
    const float cy = 140.0f;
    static lv_point_precise_t tick_segs[60][2];
    for (int i = 0; i < 60; ++i) {
        const float deg = static_cast<float>(i) * 6.0f;
        const bool major = (i % 5 == 0);
        PointAt(&tick_segs[i][0], deg, major ? 118.0f : 126.0f, cx, cy);
        PointAt(&tick_segs[i][1], deg, 132.0f, cx, cy);

        lv_obj_t* ln = lv_line_create(s.dial);
        lv_line_set_points(ln, tick_segs[i], 2);
        lv_obj_set_style_line_width(ln, major ? 3 : 1, 0);
        lv_obj_set_style_line_color(ln, major ? lv_color_hex(kBright) : lv_color_hex(kDim), 0);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    }

    // 指针（挂 dial 上，用局部坐标）
    auto make_hand = [&](uint32_t color, int width) {
        lv_obj_t* ln = lv_line_create(s.dial);
        lv_obj_set_style_line_width(ln, width, 0);
        lv_obj_set_style_line_color(ln, lv_color_hex(color), 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        return ln;
    };
    s.hand_hour = make_hand(kBright, 7);
    s.hand_min = make_hand(kAccent, 5);
    s.hand_sec = make_hand(kAccentRed, 2);
    lv_line_set_points(s.hand_hour, s.pts_hour, 2);
    lv_line_set_points(s.hand_min, s.pts_min, 2);
    lv_line_set_points(s.hand_sec, s.pts_sec, 2);

    // 中心点
    lv_obj_t* dot = lv_obj_create(s.dial);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kAccentRed), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

// ===========================================================================
// 公开接口
// ===========================================================================
lv_obj_t* Create()
{
    s.page = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s.page, lv_color_hex(kBgColor), 0);
    lv_obj_set_style_bg_opa(s.page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s.page, LV_OBJ_FLAG_SCROLLABLE);

    BuildDigital();
    BuildDial();

    // 底部提示（最后建，避免被表盘盖住）
    s.label_hint = MakeLabel(s.page, &lv_font_montserrat_14, kDim);
    lv_label_set_text(s.label_hint, "");
    lv_obj_align(s.label_hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    s.face = Face::DIGITAL;
    ApplyFace();

    // 1Hz 刷新（默认暂停）
    s.timer = lv_timer_create(
        [](lv_timer_t*) {
            if (s.page != nullptr && !lv_obj_has_flag(s.page, LV_OBJ_FLAG_HIDDEN)) {
                Refresh();
            }
        },
        1000, nullptr);
    lv_timer_pause(s.timer);

    s.last_hh = s.last_mm = s.last_sec = s.last_yday = -1;
    s.last_battery = -1;
    s.last_charging = -1;
    Refresh();
    ApplyFace();

    ESP_LOGI(TAG, "coze-style clock page created");
    return s.page;
}

void SetActive(const bool active)
{
    if (s.timer == nullptr) {
        return;
    }
    if (active) {
        s.last_sec = -1;
        Refresh();
        lv_timer_resume(s.timer);
    } else {
        lv_timer_pause(s.timer);
    }
}

void Refresh()
{
    if (s.page == nullptr) {
        return;
    }

    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    const bool time_valid = tm_now.tm_year >= 2025 - 1900;

    // 数字页：只在分钟变化时改文本
    if (time_valid && (tm_now.tm_hour != s.last_hh || tm_now.tm_min != s.last_mm)) {
        s.last_hh = tm_now.tm_hour;
        s.last_mm = tm_now.tm_min;
        char buf[8];
        // %02u 配合 unsigned，输出长度可被编译器证明
        snprintf(buf, sizeof(buf), "%.2u", static_cast<unsigned>(tm_now.tm_hour));
        lv_label_set_text(s.label_hh, buf);
        snprintf(buf, sizeof(buf), "%.2u", static_cast<unsigned>(tm_now.tm_min));
        lv_label_set_text(s.label_mm, buf);
    } else if (!time_valid) {
        lv_label_set_text(s.label_hh, "--");
        lv_label_set_text(s.label_mm, "--");
        s.last_hh = s.last_mm = -1;
    }

    // 模拟页：只在秒变化时重画指针
    if (time_valid && tm_now.tm_sec != s.last_sec) {
        s.last_sec = tm_now.tm_sec;
        if (s.face == Face::ANALOG) {
            RedrawHands(tm_now);
        }
    }

    UpdateCommonText();
}

}  // namespace clock_page_lvgl

#endif  // CONFIG_EMOTE_AS_LVGL_APP
