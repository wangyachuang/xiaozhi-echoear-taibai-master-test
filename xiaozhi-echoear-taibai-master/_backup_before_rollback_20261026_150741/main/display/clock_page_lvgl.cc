// ============================================================================
// clock_page_lvgl.cc — Timer App 页面
// ----------------------------------------------------------------------------
// 布局（360x360 圆屏，四周留 ~40px 安全区）：
//   数字面：
//     y≈100   [H][H]  :  [M][M]      四位数字，线段宽 12、高 96
//     y≈210   2026-10-23 星期五       14px 内置字体
//     y≈240   电量百分比              14px
//     y≈270   [••••••••]              秒进度条
//   模拟面：
//     圆心 (180,180)，半径 130 的表盘 + 时针/分针/秒针 + 12 个刻度
//
// 数字的线段布局（a-segment 风格，间距 gap，段长 seg_len）：
//     _a_
//    f|   |b
//     -g-
//    e|   |c
//     _d_
// ============================================================================
#include "clock_page_lvgl.h"

// 见 emote_app.cc 中的说明：必须显式包含，CONFIG_* 才可见。
#include "sdkconfig.h"

#if defined(CONFIG_LVGL_VERSION_MAJOR)

#include <esp_log.h>
#include <esp_timer.h>

#include <cmath>
#include <ctime>

#define TAG "ClockPage"

namespace clock_page_lvgl {

namespace {

constexpr int kScreenW = 360;
constexpr int kScreenH = 360;

// ---- 颜色 ----
constexpr uint32_t kBgColor = 0x0A0A0F;
constexpr uint32_t kFgColor = 0xFFFFFF;
constexpr uint32_t kDimColor = 0x4A4A55;
constexpr uint32_t kAccentColor = 0x4FC3F7;

// ---- 数字几何 ----
constexpr int kSegW = 12;        // 线段粗细
constexpr int kSegLen = 96;      // 线段长度（竖直段高度 / 水平段宽度）
constexpr int kDigitW = 60;      // 单个数字宽度
constexpr int kDigitH = 96;      // 单个数字高度
constexpr int kDigitGap = 12;    // 数字之间
constexpr int kColonGap = 26;    // 数字与冒号之间

// ---- 表盘几何 ----
constexpr int kCenterX = 180;
constexpr int kCenterY = 180;
constexpr int kFaceR = 132;

lv_obj_t* s_page = nullptr;
lv_obj_t* s_digital = nullptr;
lv_obj_t* s_analog = nullptr;    // 模拟表盘容器

lv_timer_t* s_timer = nullptr;
bool s_active = false;
bool s_analog_face = false;      // 当前是否模拟面（注意：不能与上面的 s_analog 同名）

// 数字面的四位数字 + 冒号
// 每位数字 3 个可见段（a-segment 简化版：横段 + 两竖段各表示"上/下"）
struct Seg {
    lv_obj_t* obj = nullptr;
    // 该段在"全亮"时的位置（相对数字左上角），由 BuildDigit 填充
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// 每位数字：横段 top / mid / bottom，竖段 tl / tr / bl / br
enum { S_TOP = 0, S_MID, S_BOT, S_TL, S_TR, S_BL, S_BR, S_COUNT };

Seg s_digits[4][S_COUNT];
lv_obj_t* s_colon[2] = {nullptr, nullptr};

lv_obj_t* s_date_label = nullptr;
lv_obj_t* s_battery_label = nullptr;
lv_obj_t* s_seconds_bar = nullptr;
lv_obj_t* s_meridiem_label = nullptr;

// 模拟面的指针
lv_obj_t* s_hand_hour = nullptr;
lv_obj_t* s_hand_min = nullptr;
lv_obj_t* s_hand_sec = nullptr;
lv_obj_t* s_hand_hub = nullptr;

// 数字 0-9 的段点亮表（顺序与上面的 S_* 枚举一致）
const bool kDigitMap[10][S_COUNT] = {
    //    TOP    MID    BOT    TL     TR     BL     BR
    {  true,  false, true,  true,  true,  true,  true  },  // 0
    {  false, false, false, false, true,  false, true  },  // 1
    {  true,  true,  true,  false, true,  true,  false },  // 2
    {  true,  true,  true,  false, true,  false, true  },  // 3
    {  false, true,  false, true,  true,  false, true  },  // 4
    {  true,  true,  true,  true,  false, false, true  },  // 5
    {  true,  true,  true,  true,  false, true,  true  },  // 6
    {  true,  false, false, false, true,  false, true  },  // 7
    {  true,  true,  true,  true,  true,  true,  true  },  // 8
    {  true,  true,  true,  true,  true,  false, true  },  // 9
};

const char* kWeekdayCn[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

// ---------------------------------------------------------------- 工具

lv_obj_t* MakeBar(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    // 圆角让七段看起来更"电子"
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// ---------------------------------------------------------------- 数字面

void BuildDigit(int index, lv_obj_t* parent, int origin_x, int origin_y)
{
    const int L = kSegLen;
    const int T = kSegW;

    // 三个横段：位于 0 / 中部 / 底部
    const int mid_y = (kDigitH - T) / 2;
    s_digits[index][S_TOP] = {MakeBar(parent, origin_x, origin_y, L, T, kDimColor), 0, 0, L, T};
    s_digits[index][S_MID] = {MakeBar(parent, origin_x, origin_y + mid_y, L, T, kDimColor), 0, mid_y, L, T};
    s_digits[index][S_BOT] = {MakeBar(parent, origin_x, origin_y + kDigitH - T, L, T, kDimColor), 0,
                              kDigitH - T, L, T};

    // 四根竖段：上左/上右/下左/下右
    const int upper_h = mid_y - T;
    const int lower_y = mid_y + T;
    const int lower_h = (kDigitH - T) - lower_y;
    s_digits[index][S_TL] = {MakeBar(parent, origin_x, origin_y + T, T, upper_h, kDimColor), 0, T, T, upper_h};
    s_digits[index][S_BL] = {MakeBar(parent, origin_x, origin_y + lower_y, T, lower_h, kDimColor), 0, lower_y, T,
                             lower_h};
    s_digits[index][S_TR] = {MakeBar(parent, origin_x + L - T, origin_y + T, T, upper_h, kDimColor), L - T, T, T,
                             upper_h};
    s_digits[index][S_BR] = {MakeBar(parent, origin_x + L - T, origin_y + lower_y, T, lower_h, kDimColor), L - T,
                             lower_y, T, lower_h};
}

void SetDigit(int index, int value)
{
    if (value < 0 || value > 9) {
        return;
    }
    for (int s = 0; s < S_COUNT; ++s) {
        const bool on = kDigitMap[value][s];
        if (s_digits[index][s].obj == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(s_digits[index][s].obj, lv_color_hex(on ? kFgColor : kDimColor), 0);
    }
}

void BuildDigitalFace(lv_obj_t* parent)
{
    // 四位数字 + 冒号整体居中：
    //   digit0 | gap | digit1 | colonGap | colon | colonGap | digit2 | gap | digit3
    const int colon_w = kSegW;
    const int total_w = 4 * kSegLen + 2 * kDigitGap + 2 * kColonGap + colon_w;
    const int start_x = (kScreenW - total_w) / 2;
    const int digital_y = 104;

    int x = start_x;
    // 数字 0、1（小时）
    BuildDigit(0, parent, x, digital_y);
    x += kSegLen + kDigitGap;
    BuildDigit(1, parent, x, digital_y);
    x += kSegLen + kColonGap;

    // 冒号
    s_colon[0] = MakeBar(parent, x, digital_y + kDigitH / 3 - kSegW / 2, colon_w, kSegW, kAccentColor);
    s_colon[1] = MakeBar(parent, x, digital_y + kDigitH * 2 / 3 - kSegW / 2, colon_w, kSegW, kAccentColor);
    x += colon_w + kColonGap;

    // 数字 2、3（分钟）
    BuildDigit(2, parent, x, digital_y);
    x += kSegLen + kDigitGap;
    BuildDigit(3, parent, x, digital_y);

    // 上午/下午（12 小时制的辅助显示，靠右侧竖排很短一行）
    s_meridiem_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_meridiem_label, lv_color_hex(kDimColor), 0);
    lv_label_set_text(s_meridiem_label, "AM");
    lv_obj_align(s_meridiem_label, LV_ALIGN_TOP_RIGHT, -30, 110);

    // 日期
    s_date_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(kFgColor), 0);
    lv_label_set_text(s_date_label, "---- -- --");
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 224);

    // 电量
    s_battery_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(kDimColor), 0);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_MID, 0, 250);

    // 秒进度条（60 段做成一条 240px 的细线，靠宽度随时间增长）
    const int bar_w = 240;
    const int bar_x = (kScreenW - bar_w) / 2;
    lv_obj_t* track = MakeBar(parent, bar_x, 286, bar_w, 4, kDimColor);
    lv_obj_set_style_bg_opa(track, LV_OPA_30, 0);
    s_seconds_bar = MakeBar(parent, bar_x, 286, 1, 4, kAccentColor);
    lv_obj_set_style_radius(s_seconds_bar, LV_RADIUS_CIRCLE, 0);
}

// ---------------------------------------------------------------- 模拟面

void BuildAnalogFace(lv_obj_t* parent)
{
    // 表盘外圈
    lv_obj_t* ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, kFaceR * 2, kFaceR * 2);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(kDimColor), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    // 12 个刻度：整点长、其余短
    for (int i = 0; i < 12; ++i) {
        const bool major = (i % 3 == 0);
        const int len = major ? 18 : 10;
        const int thick = major ? 4 : 2;
        lv_obj_t* tick = lv_obj_create(parent);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, thick, len);
        lv_obj_set_style_bg_color(tick, lv_color_hex(major ? kFgColor : kDimColor), 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tick, 2, 0);
        lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);

        // 刻度中心放在半径 (kFaceR - len/2 - 6) 处
        const float angle = i * 30.0f * 3.14159265f / 180.0f;
        const float r = kFaceR - len / 2.0f - 6.0f;
        const int cx = kCenterX + static_cast<int>(r * sinf(angle));
        const int cy = kCenterY - static_cast<int>(r * cosf(angle));
        lv_obj_set_pos(tick, cx - thick / 2, cy - len / 2);

        // 让刻度沿半径方向旋转
        lv_obj_set_style_transform_rotation(tick, i * 300, 0);   // 每度 10 单位
        lv_obj_set_style_transform_pivot_x(tick, thick / 2, 0);
        lv_obj_set_style_transform_pivot_y(tick, len / 2, 0);
    }

    // 指针：用固定长度的小矩形 + 旋转（pivot 放在圆心）
    auto make_pointer = [&](int len, int thick, uint32_t color) -> lv_obj_t* {
        lv_obj_t* p = lv_obj_create(parent);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, thick, len);
        lv_obj_set_style_bg_color(p, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(p, thick / 2, 0);
        // 指针从圆心向上延伸：顶部在圆心上方 len 处
        lv_obj_set_pos(p, kCenterX - thick / 2, kCenterY - len);
        // 旋转轴放在指针底部（即圆心）
        lv_obj_set_style_transform_pivot_x(p, thick / 2, 0);
        lv_obj_set_style_transform_pivot_y(p, len, 0);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
        return p;
    };

    s_hand_hour = make_pointer(70, 8, kFgColor);
    s_hand_min = make_pointer(100, 6, kFgColor);
    s_hand_sec = make_pointer(112, 2, kAccentColor);

    // 圆心
    s_hand_hub = lv_obj_create(parent);
    lv_obj_remove_style_all(s_hand_hub);
    lv_obj_set_size(s_hand_hub, 14, 14);
    lv_obj_set_pos(s_hand_hub, kCenterX - 7, kCenterY - 7);
    lv_obj_set_style_radius(s_hand_hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_hand_hub, lv_color_hex(kAccentColor), 0);
    lv_obj_set_style_bg_opa(s_hand_hub, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_hand_hub, LV_OBJ_FLAG_CLICKABLE);
}

// ---------------------------------------------------------------- 更新

void UpdateDigital(const struct tm& tm_now, bool time_valid)
{
    if (s_digital == nullptr) {
        return;
    }
    if (time_valid) {
        int hour12 = tm_now.tm_hour % 12;
        if (hour12 == 0) {
            hour12 = 12;
        }
        SetDigit(0, hour12 / 10);
        SetDigit(1, hour12 % 10);
        SetDigit(2, tm_now.tm_min / 10);
        SetDigit(3, tm_now.tm_min % 10);

        if (s_meridiem_label != nullptr) {
            lv_label_set_text(s_meridiem_label, tm_now.tm_hour < 12 ? "AM" : "PM");
        }
        if (s_date_label != nullptr) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s", tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                     tm_now.tm_mday, kWeekdayCn[tm_now.tm_wday % 7]);
            lv_label_set_text(s_date_label, buf);
        }
    } else {
        // 时间未同步：四位都显示横杠（用全灭 + 中段来表示"未知"）
        for (int i = 0; i < 4; ++i) {
            SetDigit(i, 0);
        }
        if (s_meridiem_label != nullptr) {
            lv_label_set_text(s_meridiem_label, "--");
        }
        if (s_date_label != nullptr) {
            lv_label_set_text(s_date_label, "等待同步时间");
        }
    }

    if (s_seconds_bar != nullptr) {
        const int bar_w = 240;
        const int w = time_valid ? (bar_w * (tm_now.tm_sec + 1)) / 60 : 1;
        lv_obj_set_width(s_seconds_bar, w > 0 ? w : 1);
    }
}

void UpdateAnalog(const struct tm& tm_now, bool time_valid)
{
    if (s_hand_hour == nullptr) {
        return;
    }
    // LVGL 旋转单位：0.1 度
    int hour_angle = 0;
    int min_angle = 0;
    int sec_angle = 0;
    if (time_valid) {
        const int hour12 = tm_now.tm_hour % 12;
        hour_angle = static_cast<int>((hour12 * 30 + tm_now.tm_min * 0.5f) * 10);
        min_angle = static_cast<int>((tm_now.tm_min * 6 + tm_now.tm_sec * 0.1f) * 10);
        sec_angle = tm_now.tm_sec * 60;
    }
    lv_obj_set_style_transform_rotation(s_hand_hour, hour_angle, 0);
    lv_obj_set_style_transform_rotation(s_hand_min, min_angle, 0);
    lv_obj_set_style_transform_rotation(s_hand_sec, sec_angle, 0);
}

void Refresh()
{
    time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);
    // 年份不到 2024 视为 NTP 未同步
    const bool time_valid = (tm_now.tm_year >= 2024 - 1900);

    if (s_analog_face) {
        UpdateAnalog(tm_now, time_valid);
    } else {
        UpdateDigital(tm_now, time_valid);
    }
}

void TimerCb(lv_timer_t*)
{
    if (!s_active) {
        return;   // 离开页面时即使定时器还在也不更新（双保险）
    }
    Refresh();
}

}  // namespace

lv_obj_t* Create()
{
    if (s_page != nullptr) {
        return s_page;
    }

    s_page = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(kBgColor), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    // screen 的显隐一律由 emote_app 的 lv_screen_load() 控制。
    // 不能给 screen 打 LV_OBJ_FLAG_HIDDEN：LVGL 9.3 的 lv_obj_remove_flag()
    // 处理 HIDDEN 时会 lv_obj_mark_layout_as_dirty(lv_obj_get_parent(obj))，
    // 而 screen 的 parent 恒为 NULL，会直接解引用 NULL 崩溃。

    s_digital = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_digital);
    lv_obj_set_size(s_digital, kScreenW, kScreenH);
    lv_obj_remove_flag(s_digital, LV_OBJ_FLAG_SCROLLABLE);
    BuildDigitalFace(s_digital);

    s_analog = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_analog);
    lv_obj_set_size(s_analog, kScreenW, kScreenH);
    lv_obj_remove_flag(s_analog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_analog, LV_OBJ_FLAG_HIDDEN);
    BuildAnalogFace(s_analog);

    // 1Hz 定时器：进入时启动，离开时删除，避免在桌面上空转
    ESP_LOGW(TAG, "created (digital face)");
    return s_page;
}

void SetActive(bool active)
{
    if (s_page == nullptr) {
        return;
    }
    if (active == s_active) {
        return;
    }
    s_active = active;

    if (active) {
        // 进入时总是回到数字面（对齐扣子 Timer 的默认页）
        s_analog_face = false;
        lv_obj_remove_flag(s_digital, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_analog, LV_OBJ_FLAG_HIDDEN);

        Refresh();   // 立刻补一帧，避免先闪空白
        if (s_timer == nullptr) {
            s_timer = lv_timer_create(TimerCb, 1000, nullptr);
        } else {
            lv_timer_resume(s_timer);
        }
        ESP_LOGW(TAG, "active (digital)");
    } else {
        if (s_timer != nullptr) {
            lv_timer_pause(s_timer);
        }
        ESP_LOGW(TAG, "inactive");
    }
}

bool IsActive()
{
    return s_active;
}

bool IsAnalogFace()
{
    return s_analog_face;
}

// 点击切换数字面 / 模拟面。由 emote_app 在"Timer 页 + 短按"时分发。
void ToggleFace()
{
    if (s_page == nullptr || !s_active) {
        return;
    }
    s_analog_face = !s_analog_face;
    if (s_analog_face) {
        lv_obj_add_flag(s_digital, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_analog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_analog);
    } else {
        lv_obj_remove_flag(s_digital, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_analog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_digital);
    }
    Refresh();
    ESP_LOGW(TAG, "face -> %s", s_analog_face ? "analog" : "digital");
}

}  // namespace clock_page_lvgl

#endif  // CONFIG_LVGL_VERSION_MAJOR
