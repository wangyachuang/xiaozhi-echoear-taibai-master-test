// ============================================================================
// app_launcher.cc — LVGL 桌面（App 宫格）实现
// ----------------------------------------------------------------------------
// 布局：360x360 圆屏，2 列 x 2 行宫格，每格 112x112 图标 + 名称。
//       grid 整体居中，四周留白，避开圆屏四角死角。
//
// 交互：
//   * 由 Board 的触摸任务喂入 FeedTouch()（避免和 Board 现有手势冲突）
//   * 判定"点击"：按下到抬起位移 < 12px 且时长 < 1.5s
//   * 命中图标 -> emote_app::GotoPage(page_index)
//
// 注意：本页是 LVGL 页，此时 gfx 已被挂起，所以这里可以自由用 LVGL。
// ============================================================================
#include "app_launcher.h"

#if CONFIG_EMOTE_AS_LVGL_APP

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <algorithm>
#include <vector>

#include "emote_app.h"

#define TAG "AppLauncher"

// 判定阈值
static constexpr int kTapMoveMax = 12;        // 按下到抬起的位移上限
static constexpr int64_t kTapDurationMaxUs = 1500 * 1000;

// 网格几何
static constexpr int kCellW = 130;
static constexpr int kCellH = 130;
static constexpr int kCols = 2;
static constexpr int kIconSize = 64;

namespace app_launcher {

namespace {

// AppEntry 定义在头文件的 app_launcher 命名空间里，这里引进来用
using ::app_launcher::AppEntry;

struct Cell {
    lv_obj_t* btn = nullptr;    // 可点击容器
    lv_obj_t* icon = nullptr;
    lv_obj_t* label = nullptr;
    int page_index = -1;
    lv_area_t area{};          // 绝对坐标（本页坐标系）
};

std::vector<AppEntry> s_apps;
std::vector<Cell> s_cells;
lv_obj_t* s_page = nullptr;
lv_obj_t* s_grid = nullptr;
bool s_created = false;

// 触摸状态
bool s_pressing = false;
int s_down_x = 0;
int s_down_y = 0;
int64_t s_down_us = 0;

void OnAppClicked(lv_event_t* e)
{
    auto* cell = static_cast<Cell*>(lv_event_get_user_data(e));
    if (cell == nullptr || cell->page_index < 0) {
        return;
    }
    ESP_LOGI(TAG, "launch app page %d", cell->page_index);
    emote_app::GotoPage(cell->page_index);
}

bool PointInArea(int x, int y, const lv_area_t& a)
{
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

}  // namespace

lv_obj_t* Create()
{
    if (s_created) {
        return s_page;
    }

    // 独立 screen：这是 LVGL 页，gfx 已被挂起
    s_page = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(s_page);
    lv_label_set_text(title, "喵伴");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    // 网格容器：flex 行优先，wrap 成 2 列
    s_grid = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, kCellW * kCols + 8, kCellH * 2 + 8);
    lv_obj_center(s_grid);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_grid, 6, 0);
    lv_obj_set_style_pad_column(s_grid, 6, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    // 建格
    s_cells.clear();
    for (size_t i = 0; i < s_apps.size() && i < 4; ++i) {
        const AppEntry& app = s_apps[i];

        Cell cell;
        cell.page_index = app.page_index;

        cell.btn = lv_obj_create(s_grid);
        lv_obj_remove_style_all(cell.btn);
        lv_obj_set_size(cell.btn, kCellW, kCellH);
        lv_obj_set_style_radius(cell.btn, 16, 0);
        lv_obj_set_style_bg_color(cell.btn, lv_color_hex(0x1A2430), 0);
        lv_obj_set_style_bg_opa(cell.btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(cell.btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(cell.btn, LV_OBJ_FLAG_SCROLLABLE);

        cell.icon = lv_image_create(cell.btn);
        if (app.icon != nullptr) {
            lv_image_set_src(cell.icon, app.icon);
        }
        lv_obj_set_size(cell.icon, kIconSize, kIconSize);
        lv_obj_align(cell.icon, LV_ALIGN_TOP_MID, 0, 18);

        cell.label = lv_label_create(cell.btn);
        lv_label_set_text(cell.label, app.name ? app.name : "");
        lv_obj_set_style_text_color(cell.label, lv_color_hex(0xC8D4E0), 0);
        lv_obj_set_style_text_font(cell.label, &lv_font_montserrat_14, 0);
        lv_obj_align(cell.label, LV_ALIGN_BOTTOM_MID, 0, -14);

        // 记录该格在页面上的绝对区域，供 Board 的触摸命中判定用。
        // 注意：flex 布局在 lv_obj_update_layout 之后才有真实坐标，
        //       所以在创建后强制刷新一次。
        lv_obj_update_layout(cell.btn);
        lv_obj_get_coords(cell.btn, &cell.area);

        // 先把 Cell 放进 vector（固定地址，emplace_back 返回引用），
        // 再用该引用注册事件回调。顺序不能反：先 add_event_cb 再 push_back
        // 会让回调里的 user_data 指向已被搬移的旧元素。
        Cell& stored = s_cells.emplace_back(cell);
        lv_obj_add_event_cb(cell.btn, OnAppClicked, LV_EVENT_CLICKED, &stored);
    }

    s_created = true;
    ESP_LOGI(TAG, "launcher page created, %d app(s)", (int)s_cells.size());
    return s_page;
}

bool IsActive()
{
    return s_created && s_page != nullptr && !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

void RegisterApp(const AppEntry& app)
{
    if (s_created) {
        ESP_LOGW(TAG, "RegisterApp after Create() is ignored");
        return;
    }
    s_apps.push_back(app);
}

void FeedTouch(const int pressed, const int x, const int y)
{
    if (!s_created) {
        return;
    }

    if (pressed) {
        if (!s_pressing) {
            s_pressing = true;
            s_down_x = x;
            s_down_y = y;
            s_down_us = esp_timer_get_time();
        }
        return;
    }

    if (!s_pressing) {
        return;
    }
    s_pressing = false;

    // 只有桌面可见时才处理点击
    if (!IsActive()) {
        return;
    }

    const int dx = std::abs(x - s_down_x);
    const int dy = std::abs(y - s_down_y);
    if (dx > kTapMoveMax || dy > kTapMoveMax) {
        return;   // 位移过大 -> 当作滑动，交给 emote_app 翻页
    }
    if (esp_timer_get_time() - s_down_us > kTapDurationMaxUs) {
        return;   // 长按不当作点击
    }

    for (const auto& cell : s_cells) {
        if (PointInArea(x, y, cell.area)) {
            ESP_LOGI(TAG, "tap on app (page %d)", cell.page_index);
            if (cell.page_index >= 0) {
                emote_app::GotoPage(cell.page_index);
            }
            return;
        }
    }
}

}  // namespace app_launcher

#endif  // CONFIG_EMOTE_AS_LVGL_APP
