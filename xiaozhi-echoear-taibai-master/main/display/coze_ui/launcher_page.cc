// ============================================================================
// launcher_page.cc — Coze 风格应用网格
// ============================================================================
#include "launcher_page.h"

#include "page_manager.h"

#include <esp_log.h>
#include <lvgl.h>

#include <cstring>

// Timer 图标的图形描述符。该文件（main/display/images/img_app_timer_64_64.c）
// 定义的是 C 符号，没有配套头文件，所以这里手写 extern 声明。
// 注意：它必须出现在 main/CMakeLists.txt 的 SOURCES 里才会被编译；
// 之前它不在列表里，导致图标永远退化成首字母方块（"图标没改"的根因）。
extern "C" {
extern const lv_image_dsc_t img_app_timer_64_64;
}

#define TAG "AppLauncher"

namespace coze_ui {

// 名称 -> 内建图标。Coze 的 app 各自带 launcher_icon 描述符，
// 我们用同一套思路：按 app 名字查表。
const lv_image_dsc_t* LauncherPage::FindBuiltinIcon(const char* name)
{
    if (name == nullptr) {
        return nullptr;
    }
    if (strcmp(name, "Timer") == 0) {
        return &img_app_timer_64_64;
    }
    return nullptr;
}

namespace {

// ---- 样式常量：逐项对应 esp-brookesia 360x360 dark app_launcher_data.hpp ----
constexpr int kIconCellSize   = 140;  // icon.main.size
constexpr int kIconRowPad     = 10;   // icon.main.layout_row_pad
constexpr int kImageSize      = 98;   // icon.image.default_size
constexpr int kImagePressSize = 88;   // icon.image.press_size
// 按下时的缩放百分比：88/98 ≈ 90% (LVGL 用 256 = 100%)
constexpr int kImagePressScalePct = (kImagePressSize * LV_SCALE_NONE) / kImageSize;
constexpr int kColumns        = 3;    // table.default_num
constexpr int kTableHeightPct = 50;   // table.size 高度百分比
constexpr int kDotInactive    = 12;   // indicator.spot_inactive_size
constexpr int kDotActiveW     = 40;   // indicator.spot_active_size
constexpr int kDotGap         = 10;   // indicator.main_gap
constexpr int kIndicatorYOff  = -30;  // indicator.main_align offset_y

// 每个图标对象上挂的上下文，用于事件回调里找回 target。
// 注意：这个数组必须是【文件级静态】而不是函数内静态 —— 若放在 BuildIcons()
// 里，Build 一次给每个图标分配一个槽位并让 ctx_used 自增，第二次 Build 时
// （例如切换后端后重建）槽位会被重复分配，图标彼此串台。
struct IconCtx {
    LauncherPage* self;
    UiPage* target;
    bool press_lost;   // 与 Coze 一致：拖出图标后松手不算点击
    bool press_ok;     // 本次按下已开始（CLICKED 里仅做状态复位，不做位移判定）
};

constexpr int kMaxIcons = 24;
IconCtx g_icon_ctx[kMaxIcons];
int g_icon_ctx_used = 0;

}  // namespace

LauncherPage::~LauncherPage()
{
    // root_ 的生命周期归 LVGL 屏幕所有（随 lv_display 一起销毁），
    // 这里不做 lv_obj_del，避免与 PageManager 的销毁顺序打架。
}

void LauncherPage::AddApp(const Entry& entry)
{
    entries_.push_back(entry);
    if (built_) {
        ESP_LOGW(TAG, "AddApp after Build: %s ignored for current session",
                 entry.name ? entry.name : "?");
    }
}

void LauncherPage::Build()
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

    // ---- 图标表格 ----
    table_ = lv_obj_create(root_);
    lv_obj_set_size(table_, LV_PCT(100), LV_PCT(kTableHeightPct));
    lv_obj_align(table_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(table_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(table_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(table_, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(table_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(table_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(table_, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_column(table_, 20, LV_PART_MAIN);
    lv_obj_remove_flag(table_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 底部指示点容器 ----
    indicator_ = lv_obj_create(root_);
    lv_obj_set_size(indicator_, LV_PCT(100), LV_PCT(10));
    lv_obj_align(indicator_, LV_ALIGN_BOTTOM_MID, 0, kIndicatorYOff);
    lv_obj_set_style_bg_opa(indicator_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(indicator_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(indicator_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator_, kDotGap, LV_PART_MAIN);
    lv_obj_set_flex_flow(indicator_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_SCROLLABLE);

    BuildIcons();
    UpdateIndicator();

    built_ = true;
    ESP_LOGW(TAG, "Build: %u app(s)", (unsigned)entries_.size());
}

void LauncherPage::BuildIcons()
{
    icons_.clear();
    g_icon_ctx_used = 0;   // 重新分配上下文槽位
    const int count = static_cast<int>(entries_.size());
    if (count == 0) {
        // 没有 app 时给一句提示，避免用户看到纯黑屏以为死机
        lv_obj_t* hint = lv_label_create(table_);
        lv_label_set_text(hint, "No apps");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x808080), LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < count; ++i) {
        const Entry& e = entries_[i];
        // page_manager 构建 Entry 时 icon 一律给 nullptr（它不想依赖具体图片资源），
        // 真正按名字查内建图标的工作放在这里 —— 之前只有这句注释、没有实现，
        // 所以桌面图标一直是首字母方块。
        const lv_image_dsc_t* icon = (e.icon != nullptr) ? e.icon : FindBuiltinIcon(e.name);

        lv_obj_t* cell = lv_obj_create(table_);
        lv_obj_set_size(cell, kIconCellSize, kIconCellSize);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(cell, kIconRowPad, LV_PART_MAIN);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* img_holder = lv_obj_create(cell);
        lv_obj_set_size(img_holder, kImageSize, kImageSize);
        lv_obj_set_style_bg_opa(img_holder, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(img_holder, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(img_holder, 0, LV_PART_MAIN);
        lv_obj_remove_flag(img_holder, LV_OBJ_FLAG_SCROLLABLE);

        if (icon != nullptr) {
            lv_obj_t* img = lv_image_create(img_holder);
            lv_image_set_src(img, icon);
            lv_obj_center(img);
            // 与 Coze 的 updateByNewData 等价：源图不符 98x98 时自动缩放到槽内
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);  // 点击交给 holder
            ESP_LOGW(TAG, "icon[%d] '%s': PNG asset %dx%d", i,
                     e.name ? e.name : "?", (int)icon->header.w, (int)icon->header.h);
        } else {
            // 无图标资源：退化为首字母方块，保证图标必然可见
            lv_obj_set_style_bg_color(img_holder, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(img_holder, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(img_holder, 20, LV_PART_MAIN);
            lv_obj_t* initial = lv_label_create(img_holder);
            char buf[2] = { (e.name != nullptr && e.name[0] != '\0') ? e.name[0] : '?', '\0' };
            lv_label_set_text(initial, buf);
            lv_obj_set_style_text_font(initial, &lv_font_montserrat_40, LV_PART_MAIN);
            lv_obj_set_style_text_color(initial, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_center(initial);
            // 打这行说明该 app 没拿到图标资源，正在用首字母占位方块。
            ESP_LOGW(TAG, "icon[%d] '%s': NO asset -> placeholder letter '%s'", i,
                     e.name ? e.name : "?", buf);
        }

        lv_obj_t* name = lv_label_create(cell);
        lv_label_set_text(name, (e.name != nullptr) ? e.name : "");
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

        // 事件上下文：用文件级静态数组而非 lv_malloc，避免反复进出时堆碎片。
        // 每次 Build 从头分配，保证一次 Build 内每个图标一个槽位。
        IconCtx* ctx = nullptr;
        if (g_icon_ctx_used < kMaxIcons) {
            ctx = &g_icon_ctx[g_icon_ctx_used++];
            ctx->self = this;
            ctx->target = e.target;
            ctx->press_lost = false;
            ctx->press_ok = false;
        }

        lv_obj_add_flag(img_holder, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(img_holder, PressedCb, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(img_holder, PressLostCb, LV_EVENT_PRESS_LOST, ctx);
        lv_obj_add_event_cb(img_holder, ReleasedCb, LV_EVENT_RELEASED, ctx);
        lv_obj_add_event_cb(img_holder, ClickedCb, LV_EVENT_CLICKED, ctx);

        // 把 holder 也记下来，便于后续扩展（例如统一的按下动画）。
        // 注意不要用 lv_obj_set_user_data：那是 LVGL 给控件留的内部字段，
        // 若被第三方控件实现占用，会被覆盖成脏指针。
        icons_.push_back(cell);
    }
}

void LauncherPage::UpdateIndicator()
{
    if (indicator_ == nullptr) {
        return;
    }
    // 只有一页时不显示指示点（与 Coze 的表单页行为一致）
    const int pages = (static_cast<int>(icons_.size()) + kColumns - 1) / kColumns;
    if (pages <= 1) {
        lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_HIDDEN);

    for (lv_obj_t* dot : dots_) {
        lv_obj_delete(dot);
    }
    dots_.clear();

    for (int i = 0; i < pages; ++i) {
        lv_obj_t* dot = lv_obj_create(indicator_);
        const bool active = (i == active_page_);
        lv_obj_set_size(dot, active ? kDotActiveW : kDotInactive, kDotInactive);
        lv_obj_set_style_radius(dot, kDotInactive / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(active ? 0xFFFFFF : 0xC6C6C6), LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        dots_.push_back(dot);
    }
}

void LauncherPage::OnEnter()
{
    Build();
    if (root_ != nullptr) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
    }
    ESP_LOGW(TAG, "OnEnter");
}

void LauncherPage::OnExit()
{
    if (root_ != nullptr) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGW(TAG, "OnExit");
}

bool LauncherPage::OnGesture(SwipeDir dir)
{
    // 右滑的处理交给 PageManager 默认动作（返回表情页），这里不拦。
    (void)dir;
    return false;
}

// ----------------------------------------------------------------------------
// 图标事件
// ----------------------------------------------------------------------------
void LauncherPage::PressedCb(lv_event_t* e)
{
    lv_obj_t* img_holder = lv_event_get_target_obj(e);
    if (img_holder == nullptr) {
        return;
    }
    auto* ctx = static_cast<IconCtx*>(lv_event_get_user_data(e));
    if (ctx != nullptr) {
        // press_ok 仅用于标记"本次按下已开始"，CLICKED 里不再做位移比较。
        ctx->press_ok = true;
        ctx->press_lost = false;
    }
    // 用缩放而不是改 size：flex 容器里改子对象 size 会触发整表重排，
    // 视觉上表现为图标"跳动"而不是"按下"。Coze 用的是 zoom。
    lv_obj_set_style_transform_scale(img_holder, kImagePressScalePct, LV_PART_MAIN);
    // 用 WARN 级别：本工程 CONFIG_LOG_DEFAULT_LEVEL_WARN，
    // ESP_LOGD 在串口里根本看不到，调试期一律用 W。
    ESP_LOGW(TAG, "PRESS on icon");
}

void LauncherPage::PressLostCb(lv_event_t* e)
{
    auto* ctx = static_cast<IconCtx*>(lv_event_get_user_data(e));
    lv_obj_t* img_holder = lv_event_get_target_obj(e);
    if (ctx != nullptr) {
        ctx->press_lost = true;
    }
    if (img_holder != nullptr) {
        lv_obj_set_style_transform_scale(img_holder, LV_SCALE_NONE, LV_PART_MAIN);
    }
    ESP_LOGW(TAG, "PRESS_LOST (dragged off the icon)");
}

void LauncherPage::ReleasedCb(lv_event_t* e)
{
    lv_obj_t* img_holder = lv_event_get_target_obj(e);
    if (img_holder != nullptr) {
        lv_obj_set_style_transform_scale(img_holder, LV_SCALE_NONE, LV_PART_MAIN);
    }
}

void LauncherPage::ClickedCb(lv_event_t* e)
{
    auto* ctx = static_cast<IconCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    ESP_LOGW(TAG, "CLICKED press_lost=%d", (int)ctx->press_lost);
    if (ctx->press_lost) {
        // 与 Coze 一致：按下后拖出图标再松手不算点击；复位标志供下次使用
        ctx->press_lost = false;
        ctx->press_ok = false;
        return;
    }
    // 不再做手动位移阈值判定。
    //
    // 之前用 lv_indev_get_point() 在 CLICKED 时读"当前点"去和按下点比较，
    // 但 LVGL 的 CLICKED 事件在 RELEASE 之后才派发，此时 indev 内部坐标
    // 已经被后续的触摸/释放更新过（尤其叠加 g_tap_pending 边沿锁存后），
    // 读到的点经常是陈旧的，导致"moved 7,92 / moved -4,-23"这种误判，
    // 把好好的点击拒掉 —— 这正是"有时候点得开、有时候点不开"的根因。
    //
    // LVGL 本身已经有正确的语义：CLICKED 只在 press 和 release 都命中
    // 同一个对象时触发；PRESS_LOST 在拖出图标时触发（上面已处理）。
    // 所以交给 LVGL 即可，不需要再叠一层容易出错的坐标比较。
    ctx->press_lost = false;
    ctx->press_ok = false;
    if (ctx->target != nullptr) {
        ESP_LOGW(TAG, "tap icon -> %s", ctx->target->Name());
        // OpenApp 会自己处理 LVGL 锁与外层互斥量；此时我们在 LVGL 事件回调里，
        // 已处于 lvgl_port_lock 保护之下，直接调用即可（PageManager 内部
        // 用的是递归安全的设计：GotoPage 不重新取 lvgl 锁）。
        PageManager::GetInstance().OpenApp(ctx->target);
    }
}

}  // namespace coze_ui
