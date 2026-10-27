#include "emote_display.h"

// Standard C++ headers
#include <atomic>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>

// Standard C headers
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <cstdint>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "gfx.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
// 电量百分比用的小字号字体：LVGL 内置 14px（已随 LVGL 编入固件，零额外 flash）
LV_FONT_DECLARE(lv_font_montserrat_14);

// 注意：时钟页中央大号时间曾尝试使用 font_puhui_30_4，但该 30px 字体体积约 2MB，
// 链入后 xiaozhi.bin 从 3.6MB 涨到 5.9MB，超出 ota_0/ota_1 分区（各 3.94MB）导致
// app_check_size 失败。因此时钟页统一复用已在固件中的 BUILTIN_TEXT_FONT，
// 靠放大 label 尺寸与粗体配色达到视觉上的"大号时钟"效果，不增加任何 flash 占用。

// ============================================================================
// 时钟配置（尺寸 / 几何 / 颜色 / 优化等级）已迁移到 clock_view.cc / clock_view.h
// 这里只保留和 EmoteDisplay 交互所需的接口 include。
// ============================================================================
#include "clock_view.h"
#include "lvgl_clock_probe.h"   // LVGL 时钟页探测（第 1 步验证，可整文件删除）

namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

// UI Element Names - Centralized Management
#define UI_ELEMENT_EYE_ANIM      "eye_anim"
#define UI_ELEMENT_TOAST_LABEL   "toast_label"
#define UI_ELEMENT_CLOCK_LABEL   "clock_label"
#define UI_ELEMENT_LISTEN_ANIM   "listen_anim"
#define UI_ELEMENT_STATUS_ICON   "status_icon"
#define UI_ELEMENT_CLOCK_DATE_LABEL  "clock_date_label"
// 新版时钟页（label 化 UI）元素名，见 clock_view.h
#define UI_ELEMENT_CLOCK_BATTERY_PCT "clock_battery_pct"
#define UI_ELEMENT_CLOCK_TIME_HH     "clock_time_hh"
#define UI_ELEMENT_CLOCK_TIME_MM     "clock_time_mm"
#define UI_ELEMENT_CLOCK_TIME_COLON  "clock_time_colon"
#define UI_ELEMENT_CLOCK_STEPS_PCT   "clock_steps_pct"
#define UI_ELEMENT_CLOCK_STEPS       "clock_steps"
#define UI_ELEMENT_CLOCK_MISSION     "clock_mission"
#define UI_ELEMENT_CLOCK_ARC         "clock_arc"

// Icon Names - Centralized Management
#define ICON_MIC                 "icon_mic"
#define ICON_BATTERY             "icon_Battery"
#define ICON_SPEAKER_ZZZ         "icon_speaker_zzz"
#define ICON_WIFI_FAILED         "icon_WiFi_failed"
#define ICON_WIFI_OK             "icon_wifi"
#define ICON_LISTEN              "listen"

using FlushIoReadyCallback = std::function<bool(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*)>;
using FlushCallback = std::function<void(gfx_handle_t, int, int, int, int, const void*)>;

// ============================================================================
// Global Variables
// ============================================================================

// UI element management
static gfx_obj_t* g_obj_label_toast = nullptr;
static gfx_obj_t* g_obj_label_clock = nullptr;
static gfx_obj_t* g_obj_anim_eye = nullptr;
static gfx_obj_t* g_obj_anim_listen = nullptr;
static gfx_obj_t* g_obj_img_status = nullptr;
// 表情页：电池图标右侧的电量百分比（与 g_obj_img_status 成组显示）
static gfx_obj_t* g_obj_label_battery = nullptr;

// Full-screen clock page UI objects（全新 label 化时钟页，见 clock_view.h）
static gfx_obj_t* g_obj_clock_battery_pct = nullptr;  // 顶部电池百分比
static gfx_obj_t* g_obj_clock_date = nullptr;         // 日期行
static gfx_obj_t* g_obj_clock_time_hh = nullptr;      // 时钟小时
static gfx_obj_t* g_obj_clock_time_mm = nullptr;      // 时钟分钟
static gfx_obj_t* g_obj_clock_time_colon = nullptr;   // 时钟冒号
static gfx_obj_t* g_obj_clock_steps_pct = nullptr;    // 步数完成度
static gfx_obj_t* g_obj_clock_steps = nullptr;        // 步数
static gfx_obj_t* g_obj_clock_mission[clock_view::kMissionCharCount] = {nullptr}; // 弧排文案
static gfx_obj_t* g_obj_clock_arc = nullptr;          // 进度弧
static gfx_obj_t* g_obj_clock_time_small = nullptr;   // 模拟页：顶部小号时间
static gfx_obj_t* g_obj_clock_date_right = nullptr;   // 模拟页：右侧日期
static gfx_obj_t* g_obj_clock_hands = nullptr;        // 模拟页：指针图层

// Whether the full-screen clock overlay is currently active (idle screen)
static bool g_clock_overlay_active = false;
// Whether the clock "page" is currently selected (vs. the default expression page)
static bool g_clock_page_active = false;
// Last requested emotion (so we can restore the eyes when leaving the clock page)
static std::string g_last_emotion = "neutral";

// 时钟页数据源
// 电量：改为向 Board 查询真实值（EchoEar 由 BQ27220 提供，见 bq27220.cc）。
//       读不到时显示 "--"，而不是伪造一个数字。
// 步数：暂无传感器，保留固定值占位。
static int g_clock_battery_pct = -1;      // -1 = 未知，首次成功读取后赋值
static int g_clock_steps = 1526;
static int g_clock_steps_goal = 1750;

// Track current icon to determine when to show time
static std::string g_current_icon_type = ICON_WIFI_FAILED;
static gfx_image_dsc_t g_icon_img_dsc;

// 已真正下发到 gfx 对象的图标名（用于去重）。与 g_current_icon_type 分开：
// 后者是业务语义上的"当前图标"，前者记录"已渲染的图标"，
// 避免用业务初值导致首帧 set_src 被误跳过。
static std::string g_icon_applied;


// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;
class EmoteEngine;

enum class UIDisplayMode : uint8_t {
    SHOW_LISTENING = 1,  // Show g_obj_anim_listen
    SHOW_TIME = 2,      // Show g_obj_label_clock
    SHOW_TIPS = 3,      // Show g_obj_label_toast
    SHOW_CLOCK = 4      // Show full-screen clock (digital / analog)
};

// 跟踪当前显示模式：UpdateStatusBar 每秒都会调用 SetUIDisplayMode(SHOW_TIME)（表情默认页）
// 或 clock_update_display。若不拦截重复调用，每次都会把全部对象隐藏再显示、并标记 dirty，
// 触发引擎整屏重绘 -> 在双缓冲异步刷新下表现为整屏周期性偏移/撕裂闪烁。
// 注意：SHOW_CLOCK 模式【不】拦截重复调用，因为时钟页内点击切换数字/模拟时钟时，
// ToggleClockMode 依赖再次调用 SetUIDisplayMode(SHOW_CLOCK) 来更新 digital/analog 的可见性。
static UIDisplayMode g_current_mode = static_cast<UIDisplayMode>(0);

// Forward declaration: clock rendering is delegated to the clock_view module.
static void clock_update_display();

// ============================================================================
// Helper Functions
// ============================================================================

// Function to convert align string to GFX_ALIGN enum value
char StringToGfxAlign(const std::string &align_str)
{
    static const std::unordered_map<std::string, char> align_map = {
        {"GFX_ALIGN_DEFAULT",           GFX_ALIGN_DEFAULT},
        {"GFX_ALIGN_TOP_LEFT",          GFX_ALIGN_TOP_LEFT},
        {"GFX_ALIGN_TOP_MID",           GFX_ALIGN_TOP_MID},
        {"GFX_ALIGN_TOP_RIGHT",         GFX_ALIGN_TOP_RIGHT},
        {"GFX_ALIGN_LEFT_MID",          GFX_ALIGN_LEFT_MID},
        {"GFX_ALIGN_CENTER",            GFX_ALIGN_CENTER},
        {"GFX_ALIGN_RIGHT_MID",         GFX_ALIGN_RIGHT_MID},
        {"GFX_ALIGN_BOTTOM_LEFT",       GFX_ALIGN_BOTTOM_LEFT},
        {"GFX_ALIGN_BOTTOM_MID",        GFX_ALIGN_BOTTOM_MID},
        {"GFX_ALIGN_BOTTOM_RIGHT",      GFX_ALIGN_BOTTOM_RIGHT},
        {"GFX_ALIGN_OUT_TOP_LEFT",      GFX_ALIGN_OUT_TOP_LEFT},
        {"GFX_ALIGN_OUT_TOP_MID",       GFX_ALIGN_OUT_TOP_MID},
        {"GFX_ALIGN_OUT_TOP_RIGHT",     GFX_ALIGN_OUT_TOP_RIGHT},
        {"GFX_ALIGN_OUT_LEFT_TOP",      GFX_ALIGN_OUT_LEFT_TOP},
        {"GFX_ALIGN_OUT_LEFT_MID",      GFX_ALIGN_OUT_LEFT_MID},
        {"GFX_ALIGN_OUT_LEFT_BOTTOM",   GFX_ALIGN_OUT_LEFT_BOTTOM},
        {"GFX_ALIGN_OUT_RIGHT_TOP",     GFX_ALIGN_OUT_RIGHT_TOP},
        {"GFX_ALIGN_OUT_RIGHT_MID",     GFX_ALIGN_OUT_RIGHT_MID},
        {"GFX_ALIGN_OUT_RIGHT_BOTTOM",  GFX_ALIGN_OUT_RIGHT_BOTTOM},
        {"GFX_ALIGN_OUT_BOTTOM_LEFT",   GFX_ALIGN_OUT_BOTTOM_LEFT},
        {"GFX_ALIGN_OUT_BOTTOM_MID",    GFX_ALIGN_OUT_BOTTOM_MID},
        {"GFX_ALIGN_OUT_BOTTOM_RIGHT",  GFX_ALIGN_OUT_BOTTOM_RIGHT}
    };

    const auto it = align_map.find(align_str);
    if (it != align_map.cend()) {
        return it->second;
    }

    ESP_LOGW(TAG, "Unknown align string: %s, using GFX_ALIGN_DEFAULT", align_str.c_str());
    return GFX_ALIGN_DEFAULT;
}

// ============================================================================
// EmoteEngine Class Declaration
// ============================================================================

class EmoteEngine {
public:
    EmoteEngine(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                const int width, const int height, EmoteDisplay* const display);
    ~EmoteEngine();

    void SetEyes(const std::string &emoji_name, const bool repeat, const int fps, EmoteDisplay* const display);
    void SetIcon(const std::string &icon_name, EmoteDisplay* const display);

    void* GetEngineHandle() const
    {
        return engine_handle_;
    }

    // Callback functions (public to be accessible from static helper functions)
    static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t* const edata, void* const user_ctx);
    static void OnFlush(const gfx_handle_t handle, const int x_start, const int y_start, const int x_end, const int y_end, const void* const color_data);

private:
    // 静态回调无法访问实例成员，故用静态原子量在 OnFlush（渲染上下文）与
    // OnFlushIoReady（DMA 完成 ISR）之间传递"当前块是否为整帧最后一块"。
    static std::atomic<bool> g_flush_last_block_;

    gfx_handle_t engine_handle_;
};

// 静态回调与 ISR 之间的"最后一块"标志定义（见 EmoteEngine 类内声明）
std::atomic<bool> EmoteEngine::g_flush_last_block_{false};

// ============================================================================
// UI Management Functions
// ============================================================================

static void SetUIDisplayMode(const UIDisplayMode mode, EmoteDisplay* const display)
{
    if (!display) {
        ESP_LOGE(TAG, "SetUIDisplayMode: display is nullptr");
        return;
    }

    // 同模式跳过：避免 UpdateStatusBar 每秒重复调用导致的整屏重绘（双缓冲异步下整屏偏移/闪）。
    // SHOW_CLOCK 不跳过：时钟页内切换数字/模拟时钟依赖重复调用更新可见性（见 ToggleClockMode）。
    if (mode == g_current_mode && mode != UIDisplayMode::SHOW_CLOCK) {
        return;
    }
    g_current_mode = mode;

    // gfx_obj_set_visible 内部无条件置 is_dirty，没有去重。SetUIDisplayMode 会被
    // SetStatus(STANDBY) 等路径周期性地重复调用（SHOW_CLOCK 还被刻意豁免跳过），
    // 这里用带去重的 setter，只对可见性真正变化的对象下发，避免冗余脏标记。
    auto set_visible_dedup = [](gfx_obj_t* obj, bool visible) {
        if (obj != nullptr && gfx_obj_get_visible(obj) != visible) {
            gfx_obj_set_visible(obj, visible);
        }
    };

    set_visible_dedup(g_obj_anim_listen, false);
    set_visible_dedup(g_obj_label_clock, false);
    set_visible_dedup(g_obj_label_toast, false);
    set_visible_dedup(g_obj_clock_battery_pct, false);
    set_visible_dedup(g_obj_clock_date, false);
    set_visible_dedup(g_obj_clock_time_hh, false);
    set_visible_dedup(g_obj_clock_time_mm, false);
    set_visible_dedup(g_obj_clock_time_colon, false);
    set_visible_dedup(g_obj_clock_steps_pct, false);
    set_visible_dedup(g_obj_clock_steps, false);
    set_visible_dedup(g_obj_clock_arc, false);
    // 模拟页专属对象：非时钟页一律隐藏（真正的数字/模拟可见性由
    // clock_view::ApplyFaceVisibility 在时钟页内决定）。
    set_visible_dedup(g_obj_clock_time_small, false);
    set_visible_dedup(g_obj_clock_date_right, false);
    set_visible_dedup(g_obj_clock_hands, false);
    for (int i = 0; i < clock_view::kMissionCharCount; ++i) {
        set_visible_dedup(g_obj_clock_mission[i], false);
    }
    // 时钟全屏时隐藏状态/电池图标（其默认可见，会在表盘上叠出红色充电/低电圆圈）；
    // 其它模式恢复显示。
    /**
     * 关键逻辑：从 SetUIDisplayMode 中抽出来，让 SetIcon 也能复用
     * 
     * 分离原因：SetUIDisplayMode 是由上层界面模式驱动的，而电量百分比
     * 是根据 g_current_icon_type 判断（只有当显示的是电池图标时才显示电量）。
     * 如果把百分比和图标绑定在 SetIcon 里处理，可以避免：
     *   - 每次 SetStatus 都刷一大片 UI（降低 gfx 整屏重绘频率）
     *   - 在 SHOW_CLOCK 模式下被时钟页的面板覆盖
     */
    const bool show_battery_percent = (g_current_icon_type == ICON_BATTERY);
    set_visible_dedup(g_obj_img_status, (mode != UIDisplayMode::SHOW_CLOCK));
    set_visible_dedup(g_obj_label_battery,
                      show_battery_percent && (mode != UIDisplayMode::SHOW_CLOCK));

    // Track whether the full-screen clock is the active idle view, so the
    // emoji eyes can be hidden while the clock occupies the whole screen.
    g_clock_overlay_active = (mode == UIDisplayMode::SHOW_CLOCK);
    if (g_clock_overlay_active) {
        set_visible_dedup(g_obj_anim_eye, false);
    }

    // Show the selected control
    switch (mode) {
    case UIDisplayMode::SHOW_LISTENING: {
        set_visible_dedup(g_obj_anim_listen, true);
        const AssetData emoji_data = display->GetIconData(ICON_LISTEN);
        if (emoji_data.data) {
            gfx_anim_set_src(g_obj_anim_listen, emoji_data.data, emoji_data.size);
            gfx_anim_set_segment(g_obj_anim_listen, 0, 0xFFFF, 20, true);
            gfx_anim_start(g_obj_anim_listen);
        }
        break;
    }
    case UIDisplayMode::SHOW_TIME:
        set_visible_dedup(g_obj_label_clock, true);
        break;
    case UIDisplayMode::SHOW_TIPS:
        set_visible_dedup(g_obj_label_toast, true);
        break;
    case UIDisplayMode::SHOW_CLOCK: {
        // 数字页 / 模拟页的可见性由 clock_view 按当前 ClockFace 决定，
        // 这里不再无条件显示数字页对象，否则模拟页会与数字页文字重叠。
        clock_view::ApplyFaceVisibility(g_obj_clock_battery_pct,
                                        g_obj_clock_date,
                                        g_obj_clock_time_hh, g_obj_clock_time_mm, g_obj_clock_time_colon,
                                        g_obj_clock_steps_pct, g_obj_clock_steps, g_obj_clock_mission,
                                        g_obj_clock_arc,
                                        g_obj_clock_time_small, g_obj_clock_date_right, g_obj_clock_hands);
        break;
    }
    }
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static void InitializeGraphics(const esp_lcd_panel_handle_t panel, gfx_handle_t* const engine_handle,
                               const int width, const int height)
{
    if (!panel || !engine_handle) {
        ESP_LOGE(TAG, "InitializeGraphics: Invalid parameters");
        return;
    }

    gfx_core_config_t gfx_cfg = {
        .flush_cb = EmoteEngine::OnFlush,
        .user_data = panel,
        .flags = {
            .swap = true,
            // 必须保持双缓冲：DMA 传输第 N 块的同时 CPU 可绘制第 N+1 块，
            // 两者重叠。实测对比：
            //   双缓冲   draw=29.6ms wait=18.2ms total=47.8ms
            //   单缓冲   draw=39.8ms wait=17.6ms total=57.4ms
            // 单缓冲下绘制与传输被 xEventGroupWaitBits 串行化，总耗时反而更长。
            .double_buffer = true,
            .buff_dma = true,
        },
        .h_res = static_cast<uint32_t>(width),
        .v_res = static_cast<uint32_t>(height),
        .fps = 30,
        .buffers = {
            .buf1 = nullptr,
            .buf2 = nullptr,
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = GFX_EMOTE_INIT_CONFIG()
    };

    gfx_cfg.task.task_stack_caps = MALLOC_CAP_DEFAULT;
    gfx_cfg.task.task_affinity = 0;
    gfx_cfg.task.task_priority = 5;
    gfx_cfg.task.task_stack = 8 * 1024;

    *engine_handle = gfx_emote_init(&gfx_cfg);
}

static void SetupUI(const gfx_handle_t engine_handle, EmoteDisplay* const display)
{
    if (!display) {
        ESP_LOGE(TAG, "SetupUI: display is nullptr");
        return;
    }

    gfx_emote_set_bg_color(engine_handle, GFX_COLOR_HEX(0x000000));

    g_obj_anim_eye = gfx_anim_create(engine_handle);
    gfx_obj_align(g_obj_anim_eye, GFX_ALIGN_LEFT_MID, 10, 30);
    gfx_anim_set_auto_mirror(g_obj_anim_eye, true);
    gfx_obj_set_visible(g_obj_anim_eye, false);

    g_obj_label_toast = gfx_label_create(engine_handle);
    gfx_obj_align(g_obj_label_toast, GFX_ALIGN_TOP_MID, 0, 20);
    gfx_obj_set_size(g_obj_label_toast, 200, 40);
    gfx_label_set_text(g_obj_label_toast, Lang::Strings::INITIALIZING);
    gfx_label_set_color(g_obj_label_toast, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(g_obj_label_toast, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(g_obj_label_toast, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_speed(g_obj_label_toast, 20);
    gfx_label_set_scroll_loop(g_obj_label_toast, true);
    gfx_label_set_font(g_obj_label_toast, (gfx_font_t)&BUILTIN_TEXT_FONT);

    g_obj_label_clock = gfx_label_create(engine_handle);
    gfx_obj_align(g_obj_label_clock, GFX_ALIGN_TOP_MID, 0, 15);
    gfx_obj_set_size(g_obj_label_clock, 200, 50);
    gfx_label_set_text(g_obj_label_clock, "--:--");
    gfx_label_set_color(g_obj_label_clock, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(g_obj_label_clock, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_font(g_obj_label_clock, (gfx_font_t)&BUILTIN_TEXT_FONT);

    g_obj_anim_listen = gfx_anim_create(engine_handle);
    gfx_obj_align(g_obj_anim_listen, GFX_ALIGN_TOP_MID, 0, 5);
    gfx_anim_start(g_obj_anim_listen);
    gfx_obj_set_visible(g_obj_anim_listen, false);

    g_obj_img_status = gfx_img_create(engine_handle);
    // 定位说明（这里踩过两次坑，记清楚）：
    //   1) gfx_obj_align 的 TOP_MID 公式是 x = (屏宽 - 对象宽)/2 + x_ofs，
    //      x_ofs 是"相对居中位置的偏移"，不是绝对坐标。
    //      所以统一改用 GFX_ALIGN_TOP_LEFT（x = x_ofs, y = y_ofs），给绝对坐标。
    //   2) label 内部文字从对象【顶部】开始排（gfx_draw_label.c 的 current_y 从 0 起算）。
    //      20px 字体的 line_height=25、base_line=6，文字底部还有留白，
    //      视觉重心比 y 更靠下；而 14px 字体行高更小，所以两者的 y 不能给同一个值。
    //
    // 定位说明（第三次修正，这次终于找对根因）：
    //
    // ★ 关键：电池图标的最终位置【不是】这里设的，而是被 assets 资源里的
    //   layout 配置覆盖。资源 index.json 中 status_icon 为：
    //       {"name":"status_icon","align":"GFX_ALIGN_TOP_MID","x":-100,"y":38}
    //   AddLayoutData() 会在启动时用这组值重新 align 图标。
    //   所以图标实际位置是：
    //       x = (360-30)/2 + (-100) = 65  .. 95
    //       y = 38 .. 68（中心 53）
    //   电量 label 不在 elements[] 表里，所以下面这四个常量对它是有效的。
    //   以前按 y=18..48 算居中，自然怎么调都在图标"上面"。
    //
    // 文字在框内的视觉偏移：字形实际画在 y_pos + adjusted_ofs_y，
    //   adjusted_ofs_y = line_height - base_line - box_h - ofs_y ≈ 7（14px 字体）。
    //   要让文字视觉中心对上图标中心 53：46 + 7 = 53。
    constexpr gfx_coord_t kStatusIconX = 45;
    constexpr gfx_coord_t kStatusIconY = 18;
    // 图标右边缘在 95。间距收到最紧：文字框直接贴在图标边缘上。
    // 字形本身自带 1~2px 的左侧 bearing，所以实际看是"紧贴但不相接"。
    // （上一版 97 有 2px 间隙，视觉偏松。）
    constexpr gfx_coord_t kBatteryLabelX = 95;
    constexpr gfx_coord_t kBatteryLabelY = 46;

    gfx_obj_align(g_obj_img_status, GFX_ALIGN_TOP_LEFT, kStatusIconX, kStatusIconY);

    // 电量百分比：紧贴电池图标右侧，小字号（14px），与图标垂直居中。
    g_obj_label_battery = gfx_label_create(engine_handle);
    gfx_obj_align(g_obj_label_battery, GFX_ALIGN_TOP_LEFT, kBatteryLabelX, kBatteryLabelY);
    gfx_obj_set_size(g_obj_label_battery, 56, 20);
    gfx_label_set_text(g_obj_label_battery, "");
    gfx_label_set_color(g_obj_label_battery, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(g_obj_label_battery, GFX_TEXT_ALIGN_LEFT);
    gfx_label_set_font(g_obj_label_battery, (gfx_font_t)&lv_font_montserrat_14);
    gfx_obj_set_visible(g_obj_label_battery, false);

    // Clock page UI (digital + analog) — 创建逻辑在 clock_view 模块
    clock_view::Init(engine_handle,
                     g_obj_clock_battery_pct,
                     g_obj_clock_date,
                     g_obj_clock_time_hh,
                     g_obj_clock_time_mm,
                     g_obj_clock_time_colon,
                     g_obj_clock_steps_pct,
                     g_obj_clock_steps,
                     g_obj_clock_mission,
                     g_obj_clock_arc,
                     g_obj_clock_time_small,
                     g_obj_clock_date_right,
                     g_obj_clock_hands);
    // 时钟页所有文本统一使用内置字体（大号时钟也用同一字体，靠 label 尺寸放大）
    for (int i = 0; i < clock_view::kMissionCharCount; ++i) {
        if (g_obj_clock_mission[i] != nullptr) {
            gfx_label_set_font(g_obj_clock_mission[i], (gfx_font_t)&BUILTIN_TEXT_FONT);
        }
    }
    clock_view::SetFonts(g_obj_clock_battery_pct, g_obj_clock_date,
                         g_obj_clock_time_hh, g_obj_clock_time_mm, g_obj_clock_time_colon,
                         g_obj_clock_steps_pct, g_obj_clock_steps,
                         g_obj_clock_mission,
                         g_obj_clock_time_small, g_obj_clock_date_right,
                         (void*)&BUILTIN_TEXT_FONT,
                         (void*)&BUILTIN_TEXT_FONT,
                         (void*)&BUILTIN_TEXT_FONT);

    SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, display);
}

static void RegisterCallbacks(const esp_lcd_panel_io_handle_t panel_io, const gfx_handle_t engine_handle)
{
    if (!panel_io) {
        ESP_LOGE(TAG, "RegisterCallbacks: panel_io is nullptr");
        return;
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = EmoteEngine::OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, engine_handle);
}

// ----------------------------------------------------------------------------
// 供"LVGL 时钟页探测"使用的 gfx 回调注册器（第 1 步验证用）
// ----------------------------------------------------------------------------
// 背景：panel IO 的 on_color_trans_done 只有一份，重复注册会覆盖
// （见 IDF esp_lcd_panel_io_spi.c:167-172）。切到 LVGL 时 gfx 的回调被顶掉，
// 离开时需要用本函数把回调重新注册回 gfx，表情页才能恢复正常。
static esp_lcd_panel_io_handle_t s_panel_io_for_probe = nullptr;

static void GfxCallbackRegistrar(void* engine_handle)
{
    if (s_panel_io_for_probe == nullptr) {
        ESP_LOGE(TAG, "GfxCallbackRegistrar: panel_io 未记录");
        return;
    }
    RegisterCallbacks(s_panel_io_for_probe, static_cast<gfx_handle_t>(engine_handle));
}

void EmoteDisplaySetProbePanelIo(esp_lcd_panel_io_handle_t io)
{
    s_panel_io_for_probe = io;
}
void* EmoteDisplayGetGfxCallbackRegistrar()
{
    return reinterpret_cast<void*>(&GfxCallbackRegistrar);
}

// ============================================================================
// EmoteEngine Class Implementation
// ============================================================================

EmoteEngine::EmoteEngine(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                         const int width, const int height, EmoteDisplay* const display)
{
    InitializeGraphics(panel, &engine_handle_, width, height);

    if (display) {
        gfx_emote_lock(engine_handle_);
        SetupUI(engine_handle_, display);
        gfx_emote_unlock(engine_handle_);
    }

    RegisterCallbacks(panel_io, engine_handle_);
}

EmoteEngine::~EmoteEngine()
{
    if (engine_handle_) {
        gfx_emote_deinit(engine_handle_);
        engine_handle_ = nullptr;
    }
}

void EmoteEngine::SetEyes(const std::string &emoji_name, const bool repeat, const int fps, EmoteDisplay* const display)
{
    if (!engine_handle_) {
        ESP_LOGE(TAG, "SetEyes: engine_handle_ is nullptr");
        return;
    }

    if (!display) {
        ESP_LOGE(TAG, "SetEyes: display is nullptr");
        return;
    }

    const AssetData emoji_data = display->GetEmojiData(emoji_name);
    if (emoji_data.data) {
        DisplayLockGuard lock(display);

        // ---- 去重（性能关键）----
        // gfx_anim_set_src() 内部会做两件昂贵的事：
        //   1) eaf_init() 重新校验并解析整个 EAF（confused.eaf 达 211KB，
        //      数据在 PSRAM，而 Octal PSRAM 与 SPI2(LCD) 共享时钟总线）；
        //   2) 把 current_frame 复位为 0 —— 动画被打回第一帧。
        // 因此同一个表情被重复下发时（IMU 定时切换、对话/唤醒词/状态栏
        // 多路来源都会调用 SetEmotion），动画会不断"从头重播"，
        // 表现为抽搐/卡顿，并造成高频堆重分配与总线争用。
        //
        // 注意：gfx_anim_start() 本身是幂等的（已在播放则直接返回，不会重置帧），
        // 所以这里只需跳过 set_src/set_segment；visible 与 start 仍要执行，
        // 以便"被时钟页/省电模式停掉后"能正确恢复播放。
        static std::string s_eyes_applied;
        static int  s_eyes_fps_applied = -1;
        static bool s_eyes_repeat_applied = false;

        const bool same = (s_eyes_applied == emoji_name) &&
                          (s_eyes_fps_applied == fps) &&
                          (s_eyes_repeat_applied == repeat);

        if (same) {
            // 表情未变化：只确保可见且处于播放态，不重解析、不重置帧
            gfx_obj_set_visible(g_obj_anim_eye, true);
            gfx_anim_start(g_obj_anim_eye);
            return;
        }

        gfx_anim_set_src(g_obj_anim_eye, emoji_data.data, emoji_data.size);
        gfx_anim_set_segment(g_obj_anim_eye, 0, 0xFFFF, fps, repeat);
        gfx_obj_set_visible(g_obj_anim_eye, true);
        gfx_anim_start(g_obj_anim_eye);

        s_eyes_applied = emoji_name;
        s_eyes_fps_applied = fps;
        s_eyes_repeat_applied = repeat;
    } else {
        ESP_LOGD(TAG, "SetEyes: No emoji data found for %s", emoji_name.c_str());
    }
}

void EmoteEngine::SetIcon(const std::string &icon_name, EmoteDisplay* const display)
{
    if (!engine_handle_) {
        ESP_LOGE(TAG, "SetIcon: engine_handle_ is nullptr");
        return;
    }

    if (!display) {
        ESP_LOGE(TAG, "SetIcon: display is nullptr");
        return;
    }

    // 去重：图标未变化时不重新 set_src。gfx_img_set_src 会标记对象 dirty，
    // 而引擎每轮都整屏重绘 + 交换双缓冲；SetStatus(STANDBY) 等路径会周期性地
    // 重复设置同一个图标，这里挡掉以消除冗余刷新。
    // 注意用 g_icon_applied（已渲染图标）而非 g_current_icon_type（业务当前图标），
    // 否则业务初值 ICON_WIFI_FAILED 会让首次 set_src 被误跳过、图标永远不显示。
    if (!g_icon_applied.empty() && icon_name == g_icon_applied) {
        return;
    }

    const AssetData icon_data = display->GetIconData(icon_name);
    if (icon_data.data) {
        DisplayLockGuard lock(display);

        std::memcpy(&g_icon_img_dsc.header, icon_data.data, sizeof(gfx_image_header_t));
        g_icon_img_dsc.data = static_cast<const uint8_t*>(icon_data.data) + sizeof(gfx_image_header_t);
        g_icon_img_dsc.data_size = icon_data.size - sizeof(gfx_image_header_t);

        gfx_img_set_src(g_obj_img_status, &g_icon_img_dsc);
        g_icon_applied = icon_name;
    } else {
        ESP_LOGD(TAG, "SetIcon: No icon data found for %s", icon_name.c_str());
    }
    g_current_icon_type = icon_name;
}

bool EmoteEngine::OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t* const edata,
                                 void* const user_ctx)
{
    // 真正的 DMA 传输完成回调（在 ISR 中执行）。
    // 只有在这里通知引擎"本块刷新已完成"，引擎才会安全地复用/交换帧缓冲。
    // 若在 OnFlush 里提前通知（传输尚未完成），引擎会立刻用下一块内容覆盖
    // 正在被 SPI DMA 读取的缓冲，导致画面撕裂/错位/折叠。
    // swap_act_buf 用 OnFlush 期间锁存的"是否为整帧最后一块"决定：
    // 每帧 23 块只应交换一次双缓冲，块内多次交换会使画面由不同缓冲碎片拼接。
    const gfx_handle_t handle = static_cast<gfx_handle_t>(user_ctx);
    if (handle != nullptr) {
        gfx_emote_flush_ready(handle, g_flush_last_block_.load(std::memory_order_relaxed));
    }
    return true;
}

// ----------------------------------------------------------------------------
// 后端切换：flush 覆盖表
// ----------------------------------------------------------------------------
// gfx 的 flush_cb 是 gfx_core_config_t 里的函数指针，改动它需要改配置并重建引擎。
// 这里不改 gfx 配置，而是在 OnFlush 内部加一道"转发"：
//   - 没有覆盖（正常表情）→ 走原有 esp_lcd_panel_draw_bitmap 路径
//   - 设了覆盖（LVGL 接管）→ 调用覆盖函数，gfx 不再碰 SPI
// 用 handle → 覆盖指针的小表即可，单屏场景下只有 0/1 个元素。
namespace {
constexpr int kMaxGfxHandles = 2;
std::atomic<void*> g_override_handle_[kMaxGfxHandles] = {{nullptr}, {nullptr}};
std::atomic<emote::EmoteDisplay::GfxFlushFn> g_override_fn_[kMaxGfxHandles] = {{nullptr}, {nullptr}};

void SetOverrideSlot(void* handle, emote::EmoteDisplay::GfxFlushFn fn)
{
    for (int i = 0; i < kMaxGfxHandles; ++i) {
        if (g_override_handle_[i].load() == handle) {
            g_override_fn_[i].store(fn);
            if (fn == nullptr) {
                g_override_handle_[i].store(nullptr);
            }
            return;
        }
    }
    if (fn == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxGfxHandles; ++i) {
        if (g_override_handle_[i].load() == nullptr) {
            g_override_fn_[i].store(fn);
            g_override_handle_[i].store(handle);
            return;
        }
    }
    ESP_LOGE("EmoteDisplay", "flush override slots exhausted");
}

emote::EmoteDisplay::GfxFlushFn GetOverride(void* handle)
{
    for (int i = 0; i < kMaxGfxHandles; ++i) {
        if (g_override_handle_[i].load() == handle) {
            return g_override_fn_[i].load();
        }
    }
    return nullptr;
}
}  // namespace

void EmoteDisplay::SetGfxFlushOverride(const GfxFlushFn flush)
{
    SetOverrideSlot(GetEngineHandle(), flush);
    ESP_LOGI(TAG, "gfx flush override -> %s", flush ? "suspended" : "normal");
}
void EmoteEngine::OnFlush(const gfx_handle_t handle, const int x_start, const int y_start,
                          const int x_end, const int y_end, const void* const color_data)
{
    // 先锁存"本块是否为整帧最后一块"：gfx_refr_handler 在每块 flush 前设置
    // ctx->disp.flushing_last，此处读取才有效（DMA 完成 ISR 中已失效）。
    g_flush_last_block_.store(gfx_emote_is_flushing_last(handle), std::memory_order_relaxed);

    // 后端切换：若上层设置了 flush 覆盖（LVGL 接管 panel），这里直接转交，
    // 不再发起任何 SPI 传输。等待/交换由 gfx_refr_handler 内部根据
    // flush_ready 事件位自行完成。
    const emote::EmoteDisplay::GfxFlushFn override_fn = GetOverride(handle);
    if (override_fn != nullptr) {
        override_fn(handle, x_start, y_start, x_end, y_end, color_data);
        return;
    }

    auto* const panel = static_cast<esp_lcd_panel_handle_t>(gfx_emote_get_user_data(handle));
    if (panel) {
        // 仅发起传输；完成通知交由 OnFlushIoReady（on_color_trans_done）负责。
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
    } else {
        // 无面板时也必须释放引擎等待，否则 gfx_refr_handler 会空等 20ms 超时。
        gfx_emote_flush_ready(handle, g_flush_last_block_.load(std::memory_order_relaxed));
    }
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height)
{
    panel_io_ = panel_io;
    EmoteDisplaySetProbePanelIo(panel_io);   // 供后端切换时找回 panel_io
    InitializeEngine(panel, panel_io, width, height);
#if BOARD_LVGL_CLOCK_PROBE
    // 第 1 步验证：把 panel_io 与 gfx 句柄交给探测模块，
    // 以便"离开时钟页"时能把 panel 完成回调重新注册回 gfx。
    LvglClockProbe_SetGfxCallbackRegistrar(
        reinterpret_cast<void(*)(void*)>(EmoteDisplayGetGfxCallbackRegistrar()),
        GetEngineHandle());
#endif
}

EmoteDisplay::~EmoteDisplay() = default;

void EmoteDisplay::SetEmotion(const char* const emotion)
{
        if (g_clock_overlay_active) {
        return;
    }

    // 1. 空指针防御：情绪字符串为空直接报错返回
    if (!emotion) {
        ESP_LOGE(TAG, "SetEmotion: emotion is nullptr");
        return;
    }
// 2. 调试日志：打印当前设置的情绪
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    // 3. 引擎可用性检查：引擎未初始化则直接退出
    if (!engine_) {
        return;
    }
// 4. 全局状态缓存：记录最后一次设置的情绪
    g_last_emotion = emotion;
// 5. 读取表情资源配置（是否循环、帧率）
    const AssetData emoji_data = GetEmojiData(emotion);
    bool repeat = emoji_data.loop;
    int fps = emoji_data.fps > 0 ? emoji_data.fps : 10; // 默认帧率 20fps

    if (std::strcmp(emotion, "idle") == 0 || std::strcmp(emotion, "neutral") == 0) {
       
        repeat = false;
    }

    // When the full-screen clock occupies the idle view, keep the emoji eyes hidden.
  //  if (g_clock_overlay_active) {
  //      return;
  //  }

    DisplayLockGuard lock(this);
    engine_->SetEyes(emotion, repeat, fps, this);
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    if (!engine_) {
        return;
    }

    DisplayLockGuard lock(this);
    if (content && strlen(content) > 0) {
        gfx_label_set_text(g_obj_label_toast, content);
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
    }
}

void EmoteDisplay::SetStatus(const char* const status)
{
    if (!status) {
        ESP_LOGE(TAG, "SetStatus: status is nullptr");
        return;
    }

    if (!engine_) {
        return;
    }

    DisplayLockGuard lock(this);

    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_LISTENING, this);
        engine_->SetEyes("happy", true, 20, this);
        engine_->SetIcon(ICON_MIC, this);
    } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
        engine_->SetIcon(ICON_BATTERY, this);
        if (g_clock_page_active) {
            // Clock page: full-screen clock instead of the expression UI.
            SetUIDisplayMode(UIDisplayMode::SHOW_CLOCK, this);
            clock_update_display();
        } else {
            // Default expression page: emoji eyes + small time label.
            SetUIDisplayMode(UIDisplayMode::SHOW_TIME, this);
        }
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
        engine_->SetIcon(ICON_SPEAKER_ZZZ, this);
    } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
        engine_->SetIcon(ICON_WIFI_FAILED, this);
    }

    if (std::strcmp(status, Lang::Strings::CONNECTING) != 0) {
        // 去重：toast 文本未变化时不调用 set_text。gfx_label_set_text 会标记对象
        // dirty，而引擎每轮都整屏重绘 + 交换双缓冲，冗余 set_text 会带来额外刷新。
        static std::string g_toast_last;
        if (g_toast_last != status) {
            g_toast_last = status;
            gfx_label_set_text(g_obj_label_toast, status);
        }
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    if (!notification || !engine_) {
        return;
    }
    ESP_LOGI(TAG, "ShowNotification: %s", notification);

    DisplayLockGuard lock(this);
    gfx_label_set_text(g_obj_label_toast, notification);
    SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    if (!engine_) {
        return;
    }

    // 电量：每 5 秒真正查一次（Board 侧还有 1s 缓存节流，这里再加一层，
    // 避免 gfx 每秒整屏重绘时顺带频繁走 I2C）。
    {
        static int64_t s_last_batt_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_batt_us >= 5 * 1000 * 1000) {
            s_last_batt_us = now_us;
            int level = -1;
            bool charging = false;
            bool discharging = false;
            if (Board::GetInstance().GetBatteryLevel(level, charging, discharging) && level >= 0) {
                g_clock_battery_pct = level;
            } else {
                // 读不到（未接电池/芯片掉线）→ 置未知，时钟页显示 "--"
                g_clock_battery_pct = -1;
            }
        }
    }

    // Only display time when battery icon is shown (idle state)
    DisplayLockGuard lock(this);
    if (g_current_icon_type == ICON_BATTERY) {
        if (g_clock_overlay_active) {
            // 时钟页刷新策略：
            // gfx 引擎只要有任何对象置脏就整屏重绘（实测一帧约 47.8ms）。
            // 这里【必须】每秒调用 clock_update_display()：
            //   * 模拟页：秒针要走动。
            //   * 数字页：它是唯一把 hh/mm/日期/电量/步数/进度弧推给引擎的入口，
            //     不调用则时钟停止更新。
            // 平整卡顿/错位依赖的是 clock_view::Update() 内部的逐字段去重 ——
            // 只有数值真正变化时才 set_text/set_src，未变化时对引擎零打扰。
            clock_update_display();
        } else {
            // Legacy small time label fallback.
            time_t now;
            struct tm timeinfo;
            time(&now);

            setenv("TZ", "GMT+0", 1);
            tzset();
            localtime_r(&now, &timeinfo);

            // 去重：小时间标签 "HH:MM" 在同一分钟内文本不变，每秒都 set_text 会标记
            // label dirty 触发引擎整屏重绘（双缓冲异步下整屏偏移/闪）。仅在分钟变化时更新。
            static int g_status_last_min = -1;
            if (timeinfo.tm_min != g_status_last_min) {
                g_status_last_min = timeinfo.tm_min;
                char time_str[6];
                snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                gfx_label_set_text(g_obj_label_clock, time_str);
            }

            // 电量百分比：与图标同一时刻、同一位置更新。
            // 去重：只在数值真正变化时 set_text，避免每秒标脏触发整屏重绘。
            static int g_status_last_pct = -2;   // -2 = 从未设置
            if (g_clock_battery_pct != g_status_last_pct) {
                g_status_last_pct = g_clock_battery_pct;
                char pct_str[8];
                if (g_clock_battery_pct < 0) {
                    snprintf(pct_str, sizeof(pct_str), "--");
                } else {
                    // 注意：printf 的 "%.3d" 精度表示【最少】位数，不是上限，
                    // 对 int 仍可能输出 10 位，GCC 的 -Wformat-truncation 据此报错。
                    // 这里回收成 uint8_t（最大 255，最多 3 位），
                    // 让编译器能从类型上算出上限，才是真正消除告警的写法。
                    const uint8_t pct = static_cast<uint8_t>(
                        g_clock_battery_pct > 100 ? 100 : g_clock_battery_pct);
                    snprintf(pct_str, sizeof(pct_str), "%u%%", static_cast<unsigned>(pct));
                }
                gfx_label_set_text(g_obj_label_battery, pct_str);
            }

            SetUIDisplayMode(UIDisplayMode::SHOW_TIME, this);
        }
    }
}

void EmoteDisplay::ToggleClockMode()
{
    if (!engine_) {
        return;
    }

    // 时钟页内点击：在"数字时钟 ↔ 模拟时钟"之间切换。
    // 只在时钟页可见时有意义；表情主页的点击由触摸任务走 ToggleChatState。
    DisplayLockGuard lock(this);

    if (!g_clock_overlay_active) {
        return;
    }

    clock_view::ToggleFace();
    // 切换后重新应用可见性，并立刻刷新一次新界面的内容
    SetUIDisplayMode(UIDisplayMode::SHOW_CLOCK, this);
    clock_update_display();
}

void EmoteDisplay::ToggleClockPage()
{
    if (!engine_) {
        return;
    }

    DisplayLockGuard lock(this);
    g_clock_page_active = !g_clock_page_active;

    if (g_clock_page_active) {
        // Enter clock page: full-screen clock, hide the expression eyes.
        SetUIDisplayMode(UIDisplayMode::SHOW_CLOCK, this);
        engine_->SetIcon(ICON_BATTERY, this);
        // 进入时钟页时强制时钟页所有字段下次重绘一次
        clock_view::OnPageEnter();
        clock_update_display();
        // 时钟页停掉表情动画：gfx 引擎每轮都会整屏重绘并交换双缓冲，
        // 动画对象会持续制造"无内容变化"的刷新，在无 TE 同步下表现为整屏跳动。
        // 仅暂停（不销毁），离开时钟页时由 SetEmotion 重新启动，表情逻辑不变。
        if (g_obj_anim_eye) {
            gfx_anim_stop(g_obj_anim_eye);
        }
#if BOARD_LVGL_CLOCK_PROBE
        // 第 1 步验证：切到 LVGL 绘制（会把 panel 回调改到 LVGL 侧）
        LvglClockProbe_Enter();
#endif
    } else {
        // Back to the default expression page: restore eyes with last emotion.
#if BOARD_LVGL_CLOCK_PROBE
        // 第 1 步验证：切回 gfx 绘制（重新注册 gfx 的完成回调）
        LvglClockProbe_Leave();
#endif
        SetUIDisplayMode(UIDisplayMode::SHOW_TIME, this);
        SetEmotion(g_last_emotion.c_str());
    }
}

bool EmoteDisplay::IsClockPageActive()
{
    return g_clock_page_active;
}

// Refresh the clock page by delegating to the clock_view module, which owns
// all field-level deduplication (nothing is pushed to the engine unless the
// corresponding value actually changed).
static void clock_update_display()
{
    clock_view::Update(g_obj_clock_battery_pct, g_obj_clock_date,
                       g_obj_clock_time_hh, g_obj_clock_time_mm, g_obj_clock_time_colon,
                       g_obj_clock_steps_pct, g_obj_clock_steps, g_obj_clock_mission, g_obj_clock_arc,
                       g_obj_clock_time_small, g_obj_clock_date_right, g_obj_clock_hands,
                       g_clock_battery_pct, g_clock_steps, g_clock_steps_goal);
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    if (!engine_) {
        return;
    }

    DisplayLockGuard lock(this);
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (on) {
        gfx_anim_stop(g_obj_anim_eye);
    } else {
        gfx_anim_start(g_obj_anim_eye);
    }
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
        if (engine_) {
        }
    }
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);

}
void EmoteDisplay::AddEmojiData(const std::string &name, const void* const data, const size_t size,
                                uint8_t fps, bool loop, bool lack)
{
    emoji_data_map_[name] = AssetData(data, size, fps, loop, lack);
    ESP_LOGD(TAG, "Added emoji data: %s, size: %d, fps: %d, loop: %s, lack: %s",
             name.c_str(), size, fps, loop ? "true" : "false", lack ? "true" : "false");

    DisplayLockGuard lock(this);
    if (name == "happy") {
        engine_->SetEyes("happy", loop, fps > 0 ? fps : 20, this);
    }
}

void EmoteDisplay::AddIconData(const std::string &name, const void* const data, const size_t size)
{
    icon_data_map_[name] = AssetData(data, size);
    ESP_LOGD(TAG, "Added icon data: %s, size: %d", name.c_str(), size);

    DisplayLockGuard lock(this);
    if (name == ICON_WIFI_FAILED) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
        engine_->SetIcon(ICON_WIFI_FAILED, this);
    }
}

void EmoteDisplay::AddLayoutData(const std::string &name, const std::string &align_str,
                                 const int x, const int y, const int width, const int height)
{
    const char align_enum = StringToGfxAlign(align_str);
    ESP_LOGI(TAG, "layout: %-12s | %-20s(%d) | %4d, %4d | %4dx%-4d",
             name.c_str(), align_str.c_str(), align_enum, x, y, width, height);

    struct UIElement {
        gfx_obj_t* obj;
        const char* name;
    };

    const UIElement elements[] = {
        {g_obj_anim_eye,       UI_ELEMENT_EYE_ANIM},
        {g_obj_label_toast,    UI_ELEMENT_TOAST_LABEL},
        {g_obj_label_clock,    UI_ELEMENT_CLOCK_LABEL},
        {g_obj_anim_listen,    UI_ELEMENT_LISTEN_ANIM},
        {g_obj_img_status,     UI_ELEMENT_STATUS_ICON},
        {g_obj_clock_battery_pct, UI_ELEMENT_CLOCK_BATTERY_PCT},
        {g_obj_clock_date,        UI_ELEMENT_CLOCK_DATE_LABEL},
        {g_obj_clock_time_hh,     UI_ELEMENT_CLOCK_TIME_HH},
        {g_obj_clock_time_mm,     UI_ELEMENT_CLOCK_TIME_MM},
        {g_obj_clock_time_colon,  UI_ELEMENT_CLOCK_TIME_COLON},
        {g_obj_clock_steps_pct,   UI_ELEMENT_CLOCK_STEPS_PCT},
        {g_obj_clock_steps,       UI_ELEMENT_CLOCK_STEPS},
        {g_obj_clock_arc,         UI_ELEMENT_CLOCK_ARC},
        {g_obj_clock_mission[0],  UI_ELEMENT_CLOCK_MISSION}
    };

    DisplayLockGuard lock(this);
    for (const auto &element : elements) {
        if (name == element.name && element.obj) {
            gfx_obj_align(element.obj, align_enum, x, y);
            if (width > 0 && height > 0) {
                gfx_obj_set_size(element.obj, width, height);
            }
            return;
        }
    }

    ESP_LOGW(TAG, "AddLayoutData: UI element '%s' not found", name.c_str());
}

void EmoteDisplay::AddTextFont(std::shared_ptr<LvglFont> text_font)
{
    if (!text_font) {
        ESP_LOGW(TAG, "AddTextFont: text_font is nullptr");
        return;
    }

    text_font_ = text_font;
    ESP_LOGD(TAG, "AddTextFont: Text font added successfully");

    DisplayLockGuard lock(this);
    if (g_obj_label_toast && text_font_) {
        gfx_label_set_font(g_obj_label_toast, const_cast<void*>(static_cast<const void*>(text_font_->font())));
    }
    if (g_obj_label_clock && text_font_) {
        gfx_label_set_font(g_obj_label_clock, const_cast<void*>(static_cast<const void*>(text_font_->font())));
    }
    // 电量百分比字体【故意不跟随】资源文本字体：
    //   资源字体是 20px（line_height=25），比图标高，会把 30px 图标旁的
    //   顶栏撑满、并且文字在框内靠下，视觉上像压在图标上面。
    //   这里固定用 LVGL 内置的 14px 字体（lv_font_montserrat_14，
    //   已随 LVGL 编进固件，不额外占 flash；数字和 % 都是 ASCII，够用）。
    //   注意：AddTextFont() 里不要再覆盖它。
    // 时钟页所有文本元素同样跟随文本字体
    if (text_font_) {
        gfx_obj_t* clock_labels[] = {
            g_obj_clock_battery_pct, g_obj_clock_date,
            g_obj_clock_time_hh, g_obj_clock_time_mm, g_obj_clock_time_colon,
            g_obj_clock_steps_pct, g_obj_clock_steps,
            g_obj_clock_time_small, g_obj_clock_date_right
        };
        for (auto* label : clock_labels) {
            if (label) {
                gfx_label_set_font(label, const_cast<void*>(static_cast<const void*>(text_font_->font())));
            }
        }
        for (int i = 0; i < clock_view::kMissionCharCount; ++i) {
            if (g_obj_clock_mission[i]) {
                gfx_label_set_font(g_obj_clock_mission[i], const_cast<void*>(static_cast<const void*>(text_font_->font())));
            }
        }
    }
}

AssetData EmoteDisplay::GetEmojiData(const std::string &name) const
{
    const auto it = emoji_data_map_.find(name);
    if (it != emoji_data_map_.cend()) {
        return it->second;
    }
    return AssetData();
}

AssetData EmoteDisplay::GetIconData(const std::string &name) const
{
    const auto it = icon_data_map_.find(name);
    if (it != icon_data_map_.cend()) {
        return it->second;
    }
    return AssetData();
}

EmoteEngine* EmoteDisplay::GetEngine() const
{
    return engine_.get();
}

void* EmoteDisplay::GetEngineHandle() const
{
    return engine_ ? engine_->GetEngineHandle() : nullptr;
}

void EmoteDisplay::InitializeEngine(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                                    const int width, const int height)
{
    engine_ = std::make_unique<EmoteEngine>(panel, panel_io, width, height, this);
}

bool EmoteDisplay::Lock(const int timeout_ms)
{
    if (engine_ && engine_->GetEngineHandle()) {
        gfx_emote_lock(engine_->GetEngineHandle());
        return true;
    }
    return false;
}

void EmoteDisplay::Unlock()
{
    if (engine_ && engine_->GetEngineHandle()) {
        gfx_emote_unlock(engine_->GetEngineHandle());
    }
}

} // namespace emote
