// ============================================================================
// app_launcher.cc — LVGL 桌面（App 宫格）
// ----------------------------------------------------------------------------
// 说明：
//   1) 本模块只负责"画宫格 + 命中点击"，不持有面板、不发 SPI。
//      面板归属由 emote_app 的 ActivateGfx/ActivateLvgl 决定。
//   2) 点击判定放在这里而不是交给 LVGL 的事件系统，是因为触摸由 Board 的
//      touch_event_task 统一采集（同一份数据还要做滑动手势），
//      直接把原始坐标喂进来最省事，也避免两个消费者抢同一次触摸。
//   3) 宫格布局：2 列的 flex wrap，图标 64x64，文字 14px（LVGL 内置字体，
//      当前 sdkconfig 只开了 CONFIG_LV_FONT_MONTSERRAT_14）。
// ============================================================================
#include "app_launcher.h"

// 见 emote_app.cc 中的说明：必须显式包含，CONFIG_* 才可见。
#include "sdkconfig.h"

#if defined(CONFIG_LVGL_VERSION_MAJOR)

#include <esp_log.h>
#include <esp_timer.h>

#include <cstdlib>   // std::abs（不要依赖其它头文件间接带入）
#include <ctime>     // localtime_r / strftime
#include <vector>

#define TAG "AppLauncher"

namespace app_launcher {

namespace {

constexpr int kScreenW = 360;
constexpr int kScreenH = 360;

// 点击判定：位移不超过该像素且持续时间不超过该毫秒
constexpr int kTapMaxMove = 24;
constexpr int64_t kTapMaxUs = 800 * 1000;

std::vector<AppEntry> s_apps;
lv_obj_t* s_page = nullptr;
lv_obj_t* s_grid = nullptr;
bool s_active = false;

LaunchCallback s_launch = nullptr;

// 当前这次按下的状态
bool s_pressed = false;
int s_down_x = 0;
int s_down_y = 0;
int64_t s_down_us = 0;

// 时间显示在桌面顶部，和表情页的小时间标签呼应
lv_obj_t* s_clock_label = nullptr;
lv_timer_t* s_clock_timer = nullptr;   // 只有桌面可见时才运行

void RefreshClockLabel()
{
    if (s_clock_label == nullptr) {
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);
    char buf[16];
    // 年份不到 2024 说明 NTP 还没同步，显示占位
    if (tm_now.tm_year >= 2024 - 1900) {
        strftime(buf, sizeof(buf), "%H:%M", &tm_now);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    lv_label_set_text(s_clock_label, buf);
}

void ClockTimerCb(lv_timer_t*)
{
    RefreshClockLabel();
}

// 在按下点找到命中的 App。直接按布局几何算，不依赖 LVGL 的 hit test，
// 这样即使 LVGL 正在绘制也安全（不会访问正在被改动的对象树）。
int HitTest(int x, int y)
{
    if (s_apps.empty()) {
        return -1;
    }

    // 与 BuildGrid 的布局保持一致：
    //   图标 64x64，文字 14px 在图标下方，每个单元宽 128、高 128
    constexpr int kCellW = 128;
    constexpr int kCellH = 128;
    constexpr int kCols = 2;
    constexpr int kGapX = 20;
    constexpr int kGapY = 16;

    const int rows = (static_cast<int>(s_apps.size()) + kCols - 1) / kCols;
    const int grid_w = kCols * kCellW + (kCols - 1) * kGapX;
    const int grid_h = rows * kCellH + (rows - 1) * kGapY;
    const int origin_x = (kScreenW - grid_w) / 2;
    // 网格整体略下移，给顶部时间留位置
    const int origin_y = (kScreenH - grid_h) / 2 + 24;

    const int lx = x - origin_x;
    const int ly = y - origin_y;
    if (lx < 0 || ly < 0) {
        return -1;
    }
    const int col = lx / (kCellW + kGapX);
    const int row = ly / (kCellH + kGapY);
    if (col < 0 || col >= kCols || row < 0 || row >= rows) {
        return -1;
    }
    // 落在单元间隙里不算命中
    if (lx - col * (kCellW + kGapX) > kCellW || ly - row * (kCellH + kGapY) > kCellH) {
        return -1;
    }
    const int index = row * kCols + col;
    if (index < 0 || index >= static_cast<int>(s_apps.size())) {
        return -1;
    }
    return index;
}

void BuildGrid()
{
    if (s_page == nullptr) {
        ESP_LOGW(TAG, "BuildGrid skipped: page is null");
        return;
    }
    ESP_LOGW(TAG, "BuildGrid: %d app(s)", static_cast<int>(s_apps.size()));
    if (s_grid != nullptr) {
        lv_obj_delete(s_grid);
        s_grid = nullptr;
    }

    s_grid = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_grid, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    // flex 容器必须给定宽度，否则 ROW_WRAP 会排成一行
    lv_obj_set_width(s_grid, 2 * 128 + 20);
    lv_obj_set_style_pad_column(s_grid, 20, 0);
    lv_obj_set_style_pad_row(s_grid, 16, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (const auto& app : s_apps) {
        lv_obj_t* cell = lv_obj_create(s_grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 128, 128);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        // 点击由 FeedTouch 统一处理，这里不让 LVGL 抢焦点
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* icon = lv_image_create(cell);
        lv_image_set_src(icon, app.icon);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t* label = lv_label_create(cell);
        lv_label_set_text(label, app.name);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);

        lv_obj_move_foreground(icon);
    }
}

}  // namespace

void SetLaunchCallback(LaunchCallback cb)
{
    s_launch = cb;
}

void RegisterApp(const AppEntry& app)
{
    s_apps.push_back(app);
    if (s_page != nullptr) {
        BuildGrid();
    }
}

lv_obj_t* Create()
{
    if (s_page != nullptr) {
        return s_page;
    }

    // 独立 screen：必须 NULL 父对象才会被 lv_screen_load 真正当成一页
    s_page = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x0A0A0F), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    // 注意：绝不能对 screen 用 LV_OBJ_FLAG_HIDDEN 控制显隐。
    // LVGL 9.3 的 lv_obj_remove_flag 在 HIDDEN 分支会执行
    //   lv_obj_mark_layout_as_dirty(lv_obj_get_parent(obj))
    // 而 screen 的 parent 恒为 NULL，于是解引用 NULL 触发
    // LoadProhibited（EXCVADDR=0x2a，即 parent 字段偏移）。
    // 页面可见性一律交给 lv_screen_load()。因此这里不加 HIDDEN 标记。

    // 顶部时间（与表情页的小时间标签同一风格）
    s_clock_label = lv_label_create(s_page);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(s_clock_label, "--:--");

    s_clock_timer = lv_timer_create(ClockTimerCb, 1000, nullptr);
    if (s_clock_timer != nullptr) {
        // 创建即暂停，进入桌面时再恢复，离开时暂停。
        lv_timer_pause(s_clock_timer);
    }
    RefreshClockLabel();

    BuildGrid();
    ESP_LOGW(TAG, "launcher created, %d app(s)", static_cast<int>(s_apps.size()));
    return s_page;
}

bool IsActive()
{
    return s_active;
}

void OnEnter()
{
    if (s_page == nullptr) {
        return;
    }
    s_active = true;
    s_pressed = false;
    // 显隐由 emote_app 的 lv_screen_load() 负责，这里只刷新内容。
    // （对 screen 调 lv_obj_remove_flag(HIDDEN) 会因 parent==NULL 崩溃，
    //  详见 Create() 里的说明。）
    lv_obj_invalidate(s_page);
    RefreshClockLabel();
    if (s_clock_timer != nullptr) {
        lv_timer_resume(s_clock_timer);
    }
    ESP_LOGW(TAG, "enter");
}

void OnLeave()
{
    s_active = false;
    s_pressed = false;
    if (s_clock_timer != nullptr) {
        lv_timer_pause(s_clock_timer);
    }
    ESP_LOGW(TAG, "leave");
}

bool FeedTouch(bool pressed, int x, int y)
{
    if (!s_active) {
        return false;
    }

    if (pressed) {
        s_pressed = true;
        s_down_x = x;
        s_down_y = y;
        s_down_us = esp_timer_get_time();
        return false;   // 按下不消费，让上层继续记录滑动起点
    }

    if (!s_pressed) {
        return false;
    }
    s_pressed = false;

    const int64_t held_us = esp_timer_get_time() - s_down_us;
    const int dx = x - s_down_x;
    const int dy = y - s_down_y;

    // 位移大或按得久 → 不是点击，交还上层做滑动
    if (std::abs(dx) > kTapMaxMove || std::abs(dy) > kTapMaxMove || held_us > kTapMaxUs) {
        return false;
    }

    // 以抬起点为准（和按下点差别已经在 kTapMaxMove 之内）
    const int hit = HitTest(x, y);
    if (hit < 0) {
        return false;   // 点到空白处：不消费，交给上层的"点屏唤醒对话"
    }

    const int page_index = s_apps[hit].page_index;
    ESP_LOGW(TAG, "tap app '%s' -> page %d", s_apps[hit].name, page_index);
    if (s_launch != nullptr) {
        s_launch(page_index);
    }
    return true;
}

}  // namespace app_launcher

#endif  // CONFIG_LVGL_VERSION_MAJOR
