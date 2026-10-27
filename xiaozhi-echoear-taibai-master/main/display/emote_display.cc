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

// Last requested emotion (so we can restore it when needed)
static std::string g_last_emotion = "neutral";

// 电量百分比（表情页 label 显示用）。-1 = 未知（未接电池 / 芯片掉线），显示 "--"。
static int g_battery_pct = -1;

// 状态条两处去重的"上次已写入值"。
// 必须是文件作用域（而不是 UpdateStatusBar 里的函数内 static），
// 这样 InvalidateDedupCache() 才能在引擎重建后把它们清空。
static int g_status_last_min = -1;    // 上次写入时间标签的分钟数，-1 = 未写入
static int g_status_last_pct = -2;    // 上次写入的电量百分比，-2 = 从未设置

// ----------------------------------------------------------------------------
// 桌面（LVGL 页面）激活标志
//
// 背景：gfx_core.c 的 gfx_object_handler() 只要对象树非空，每轮都会整屏重绘，
// 且 panel_io 的 on_color_trans_done 只有一个槽位。要让 LVGL 桌面独占屏幕，
// 仅仅把 flush 转发掉（SetGfxFlushOverride）不够：gfx 仍会持续产生脏标记、
// 每轮走完整个渲染管线，白白烧 CPU，还可能因为 flush_ready 时序错乱而卡死。
//
// 因此这里再加一道"闸门"：桌面激活期间，所有会推进 gfx 状态的入口
// （SetEmotion / SetStatus / SetChatMessage / ShowNotification / UpdateStatusBar）
// 直接返回，一个脏标记都不产生，gfx 任务彻底空转。
// 离开桌面时解除闸门，并用 g_last_emotion / g_current_icon_type 恢复现场。
// ----------------------------------------------------------------------------
static volatile bool g_desktop_active = false;

// 相邻两次"离开桌面"之间至少间隔这么久才做全量恢复，
// 避免用户反复左右滑动时每滑一次就整屏重绘一次。
static constexpr int64_t kDesktopRecoverMinIntervalUs = 1500 * 1000;
static int64_t g_last_desktop_exit_us = 0;

// Track current icon to determine when to show time
static std::string g_current_icon_type = ICON_WIFI_FAILED;
static gfx_image_dsc_t g_icon_img_dsc;

// 已真正下发到 gfx 对象的图标名（用于去重）。与 g_current_icon_type 分开：
// 后者是业务语义上的"当前图标"，前者记录"已渲染的图标"，
// 避免用业务初值导致首帧 set_src 被误跳过。
static std::string g_icon_applied;

// 已真正下发到 gfx 的眼睛动画参数（用于去重）。
// 原本是 SetEyes 内部静态变量，提升到文件作用域是为了支持
// EmoteEngine::InvalidateDedupCache() —— 桌面切回表情时需要强制重绘一次。
static std::string g_eyes_applied;
static int  g_eyes_fps_applied = -1;
static bool g_eyes_repeat_applied = false;


// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;
class EmoteEngine;

enum class UIDisplayMode : uint8_t {
    SHOW_LISTENING = 1,  // Show g_obj_anim_listen
    SHOW_TIME = 2,      // Show g_obj_label_clock
    SHOW_TIPS = 3       // Show g_obj_label_toast
};

// 跟踪当前显示模式：UpdateStatusBar 每秒都会调用 SetUIDisplayMode(SHOW_TIME)（表情默认页）
// 若不拦截重复调用，每次都会把全部对象隐藏再显示、并标记 dirty，
// 触发引擎整屏重绘 -> 在双缓冲异步刷新下表现为整屏周期性偏移/撕裂闪烁。
static UIDisplayMode g_current_mode = static_cast<UIDisplayMode>(0);

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

    // 清空 SetEyes / SetIcon 的"已应用"去重缓存。
    // 用于桌面切回表情页时强制重新下发一次 set_src，避免
    // "被挡住的这段时间里状态已变、恢复时又被去重跳过"导致画面不刷新。
    static void InvalidateDedupCache();

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
    if (mode == g_current_mode) {
        return;
    }
    g_current_mode = mode;

    // gfx_obj_set_visible 内部无条件置 is_dirty，没有去重。SetUIDisplayMode 会被
    // SetStatus(STANDBY) 等路径周期性地重复调用，
    // 这里用带去重的 setter，只对可见性真正变化的对象下发，避免冗余脏标记。
    auto set_visible_dedup = [](gfx_obj_t* obj, bool visible) {
        if (obj != nullptr && gfx_obj_get_visible(obj) != visible) {
            gfx_obj_set_visible(obj, visible);
        }
    };

    set_visible_dedup(g_obj_anim_listen, false);
    set_visible_dedup(g_obj_label_clock, false);
    set_visible_dedup(g_obj_label_toast, false);

    /**
     * 关键逻辑：从 SetUIDisplayMode 中抽出来，让 SetIcon 也能复用
     * 
     * 分离原因：SetUIDisplayMode 是由上层界面模式驱动的，而电量百分比
     * 是根据 g_current_icon_type 判断（只有当显示的是电池图标时才显示电量）。
     * 如果把百分比和图标绑定在 SetIcon 里处理，可以避免：
     *   - 每次 SetStatus 都刷一大片 UI（降低 gfx 整屏重绘频率）
     */
    const bool show_battery_percent = (g_current_icon_type == ICON_BATTERY);
    set_visible_dedup(g_obj_img_status, true);
    set_visible_dedup(g_obj_label_battery, show_battery_percent);

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
    }
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

// ----------------------------------------------------------------------------
// gfx 引擎的帧缓冲：持久化 + 放 PSRAM
// ----------------------------------------------------------------------------
// 背景（"Failed to allocate frame buffer 2" 的真实原因）：
//   进桌面时 gfx 引擎被销毁（route B），LVGL 侧会长期占用一块
//   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL 的显示缓冲（360 x 40 行 x 2B = 28800 B，
//   见 page_manager.cc 的 EnsureLvglDisplay）。而 gfx 引擎原本用
//   flags.buff_dma = true 在【内部 DMA RAM】里分配双缓冲
//   （buf_pixels = 360*16 = 5760 px * 2B * 2 = 23040 B）。
//   两块内部 DMA 内存同时存在，第二次来回切换时内部 RAM 就分不出第二块缓冲，
//   于是 gfx_emote_init 返回空句柄，EmoteDisplay 打印
//   "desktop inactive: cannot rebuild engine (..., handle=0)"，
//   表情页再也回不去 —— 表现为"滑动退不出桌面 + 满屏红色报错"。
//
// 修法：帧缓冲改为【外部传入】（cfg.buffers.buf1 != NULL 时 gfx 组件
//   不做任何分配，见 gfx_core.c:155-166 的 ext_bufs 分支），由我们自己在
//   PSRAM 里分配一次并永久复用。这样：
//     * 引擎重建多少次都不会再碰内部 RAM，彻底消除分配失败；
//     * 不再随引擎销毁/重建反复 malloc/free，也就没有碎片问题；
//     * PSRAM 容量充足，双缓冲常驻没有代价。
//   注意不要同时置 buff_dma，gfx_core.c:170-174 会直接返回 ESP_ERR_NOT_SUPPORTED。
static uint16_t* s_frame_buf = nullptr;   // [2] 双缓冲，静态复用
static size_t    s_frame_buf_pixels = 0;

static void* AllocFrameBuffers(const size_t total_pixels)
{
    if (s_frame_buf != nullptr && s_frame_buf_pixels >= total_pixels) {
        return s_frame_buf;   // 已分配且够大，直接复用
    }
    if (s_frame_buf != nullptr) {
        heap_caps_free(s_frame_buf);
        s_frame_buf = nullptr;
        s_frame_buf_pixels = 0;
    }
    const size_t bytes = total_pixels * sizeof(uint16_t);
    // 优先 PSRAM（引擎重建时不会与 LVGL 争内部 DMA RAM）。
    // 必须带 MALLOC_CAP_DMA：SPI 直接从这块内存取数，地址要被 DMA 引擎接受。
    // 本板 CONFIG_SOC_PSRAM_DMA_CAPABLE=y（OCT 模式 80MHz），PSRAM 可 DMA；
    // 若某些板子不支持，这次分配会失败，下面自动退回内部 DMA RAM。
    uint16_t* p = static_cast<uint16_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    if (p == nullptr) {
        ESP_LOGW(TAG, "PSRAM frame buffers unavailable, falling back to internal RAM");
        p = static_cast<uint16_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    if (p == nullptr) {
        ESP_LOGE(TAG, "frame buffers alloc failed (%u B)", static_cast<unsigned>(bytes));
        return nullptr;
    }
    s_frame_buf = p;
    s_frame_buf_pixels = total_pixels;
    ESP_LOGW(TAG, "frame buffers allocated: %u B (2x %u px)",
             static_cast<unsigned>(bytes), static_cast<unsigned>(total_pixels / 2));
    return s_frame_buf;
}

static void InitializeGraphics(const esp_lcd_panel_handle_t panel, gfx_handle_t* const engine_handle,
                               const int width, const int height)
{
    if (!panel || !engine_handle) {
        ESP_LOGE(TAG, "InitializeGraphics: Invalid parameters");
        return;
    }

    // 双缓冲 -> buf1/buf2 各 buf_pixels；总像素 = 2 * buf_pixels。
    // 与原配置 buf_pixels = width * 16 保持一致（360x360 屏幕即 5760 px，
    // 每块 11520 B，双缓冲共 23040 B）。
    const size_t kBufPixelsPerBuffer = static_cast<size_t>(width) * 16;
    uint16_t* const ext_buf = static_cast<uint16_t*>(
        AllocFrameBuffers(kBufPixelsPerBuffer * 2));
    if (ext_buf == nullptr) {
        // 分配失败就让 gfx 自己走原来的内部 RAM 路径，至少还有一半机会成功。
        ESP_LOGW(TAG, "using gfx built-in buffer allocation (external alloc failed)");
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
            // 帧缓冲由我们在 PSRAM 里分配并外部传入（见上面的 AllocFrameBuffers）。
            // 因此这两个 flag 必须都为 false：既不需要 gfx 自己分配，
            // 也不能声明成 DMA/SPIRAM 内部缓冲（gfx_core.c:170-174 会因为
            // buff_dma && buff_spiram 直接返回 ESP_ERR_NOT_SUPPORTED）。
            .buff_dma = false,
            .buff_spiram = false,
        },
        .h_res = static_cast<uint32_t>(width),
        .v_res = static_cast<uint32_t>(height),
        .fps = 30,
        .buffers = {
            // buf1 非空即触发 gfx_core.c:155-166 的 ext_bufs 分支（完全不分配）。
            // 分配失败时全部置空，让 gfx 退回自己 malloc 的旧路径。
            .buf1 = ext_buf,
            .buf2 = (ext_buf != nullptr) ? (ext_buf + kBufPixelsPerBuffer) : nullptr,
            .buf_pixels = (ext_buf != nullptr) ? kBufPixelsPerBuffer : 0,
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
    // 注意：这里不能提前 gfx_anim_start —— 此时还没 set_src，
    // gfx 会报 "Animation source not set"。等真正设置动画源的那条路径
    // （SetUIDisplayMode → SHOW_LISTENING）再启动即可。
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
    // ★★ 关键（第四次修正，桌面切回后图标跑到左上角就是这里）：
    //
    //   电池图标的位置有两个来源，必须一致：
    //     A) 下面这几行 —— SetupUI() 每次【重建引擎】都会执行；
    //     B) main/boards/echoear/layout.json 里的 status_icon 布局 ——
    //        由 main/assets.cc:370 在【启动时】调用 AddLayoutData() 应用一次。
    //
    //   以前 A 写的是 (45,18)、B 是 TOP_MID/-100/38（等价于 65,38），两者不同。
    //   启动时 B 覆盖了 A，所以看起来一切正常；但桌面切换会销毁并重建 gfx 引擎，
    //   重建后只有 A 生效，B 不会重跑 —— 图标就跳到了 A 的老坐标 (45,18)，
    //   也就是左上角。这就是"左滑进桌面再右滑回来，电池图标偏到左上角"的根因。
    //
    //   现在让 A 与 B 完全等价：用 TOP_MID + (-100, 38)，
    //   与 layout.json 逐字对齐，两处谁生效结果都一样。
    //   ！！！以后要挪电池图标，layout.json 和这里【必须同时改】！！！
    //
    //   资源里的实际位置（icon 宽 30、高 30）：
    //       x = (360-30)/2 + (-100) = 65  .. 95
    //       y = 38 .. 68（中心 53）
    //   电量 label 不在 layout.json 的 elements 表里，只有下面这组常量对它有效。
    //
    //   文字在框内的视觉偏移：字形实际画在 y_pos + adjusted_ofs_y，
    //     adjusted_ofs_y = line_height - base_line - box_h - ofs_y ≈ 7（14px 字体）。
    //   要让文字视觉中心对上图标中心 53：46 + 7 = 53。
    constexpr gfx_coord_t kStatusIconX = -100;   // 与 layout.json 的 status_icon.x 一致
    constexpr gfx_coord_t kStatusIconY = 38;     // 与 layout.json 的 status_icon.y 一致
    // 图标右边缘在 95。间距收到最紧：文字框直接贴在图标边缘上。
    // 字形本身自带 1~2px 的左侧 bearing，所以实际看是"紧贴但不相接"。
    // （上一版 97 有 2px 间隙，视觉偏松。）
    constexpr gfx_coord_t kBatteryLabelX = 95;
    constexpr gfx_coord_t kBatteryLabelY = 46;

    gfx_obj_align(g_obj_img_status, GFX_ALIGN_TOP_MID, kStatusIconX, kStatusIconY);

    // 电量百分比：紧贴电池图标右侧，小字号（14px），与图标垂直居中。
    g_obj_label_battery = gfx_label_create(engine_handle);
    gfx_obj_align(g_obj_label_battery, GFX_ALIGN_TOP_LEFT, kBatteryLabelX, kBatteryLabelY);
    gfx_obj_set_size(g_obj_label_battery, 56, 20);
    gfx_label_set_text(g_obj_label_battery, "");
    gfx_label_set_color(g_obj_label_battery, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(g_obj_label_battery, GFX_TEXT_ALIGN_LEFT);
    gfx_label_set_font(g_obj_label_battery, (gfx_font_t)&lv_font_montserrat_14);
    gfx_obj_set_visible(g_obj_label_battery, false);

    SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, display);
}

// 销毁 UI 对象。
//
// 为什么必须自己做：gfx_emote_deinit()（gfx_core.c:447-454）只释放了
// child_list 的链表节点，**从不释放 child_node->src 指向的对象本身**。
// 而 gfx_anim_create / gfx_label_create / gfx_img_create 都是
// malloc(sizeof(gfx_obj_t)) 挂在 child_list 上的。于是每进出一次桌面
// 就泄漏整套 6 个对象（含 label 的 text / font_ctx / mask、
// anim 的 timer / frame / file_desc）。
// 实测症状：第 1、2 次切换正常，第 3 次开始
//   E gfx_core: Failed to allocate frame buffer 2
//   之后 SetEyes/SetIcon 全部 engine_handle_ is nullptr，画面彻底不动。
//
// gfx_obj_delete() 会做完整清理并把自己从 child_list 摘掉，
// 所以随后 gfx_emote_deinit() 不会再看到它们，不会 double free。
static void TeardownUI()
{
    // 顺序无关紧要，但先停动画更稳：避免删除过程中还有回调在跑。
    gfx_obj_t** const objs[] = {
        &g_obj_anim_eye, &g_obj_anim_listen,
        &g_obj_label_toast, &g_obj_label_clock, &g_obj_label_battery,
        &g_obj_img_status,
    };
    for (gfx_obj_t** slot : objs) {
        if (*slot != nullptr) {
            gfx_obj_delete(*slot);
            *slot = nullptr;
        }
    }
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
        // 去重状态在文件作用域（g_eyes_*），见 InvalidateDedupCache()。
        const bool same = (g_eyes_applied == emoji_name) &&
                          (g_eyes_fps_applied == fps) &&
                          (g_eyes_repeat_applied == repeat);

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

        g_eyes_applied = emoji_name;
        g_eyes_fps_applied = fps;
        g_eyes_repeat_applied = repeat;
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
        // 去重命中时【仍然】要同步业务侧记录的当前图标类型。
        // 原来这一行写在下面对 return 之后，命中时直接跳过了它，
        // 导致 g_current_icon_type 永远停在旧值 —— 表现就是
        // "从桌面滑回来以后电池图标不见了"：状态条按 g_current_icon_type
        // 判断要不要画图标/时间，它错了，整条状态条就跟着错。
        g_current_icon_type = icon_name;
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

// ----------------------------------------------------------------------------
// 去重缓存失效（供桌面切回表情时强制重绘一次）
// ----------------------------------------------------------------------------
// SetEyes / SetIcon 各自有一个"已应用"的静态去重记录。原本它们写在函数内部，
// 外部无法清空；为了让"从桌面切回表情"能强制重新下发一次 set_src，
// 这里把两个记录提升到文件作用域，由本函数统一清空。
void EmoteEngine::InvalidateDedupCache()
{
    g_eyes_applied.clear();
    g_eyes_fps_applied = -1;
    g_eyes_repeat_applied = false;
    g_icon_applied.clear();
    // 状态条的两处去重也必须清空。
    // UpdateStatusBar 用"分钟没变就不写时间""电量没变就不写百分比"来省重绘，
    // 但引擎重建后 g_obj_label_clock / g_obj_label_battery 是全新的空 label。
    // 如果这两个缓存还记着旧值，重建后就永远不会再写文本 ——
    // 这正是"右滑回表情页后时间和电量消失"的原因。
    g_status_last_min = -1;
    g_status_last_pct = -2;   // -2 = 从未设置，与 UpdateStatusBar 内初值保持一致
}

// ----------------------------------------------------------------------------
// 面板传输归属标记（必须先于 OnFlushIoReady 定义）
// ----------------------------------------------------------------------------
// 这块屏的 panel_io 只有一个 on_color_trans_done 槽位，而 gfx 与 LVGL 需要
// 共用它。历史教训（反复踩过）：
//   * 谁后注册谁拥有 —— LVGL 的 esp_lvgl_port 会无条件重注册并把 gfx 顶掉；
//   * 顶掉后 gfx 的 flush_ready 永远等不到 → 表情一帧一帧地卡；
//   * 反过来 LVGL 的 flush_ready 也依赖这个槽位，两边互相踩谁都收不全。
//
// 因此现在的约定是：**这个槽位永远只属于 gfx**（EmoteEngine::OnFlushIoReady），
// 不再允许任何模块重注册。谁发起 SPI 传输，谁就在发起前把 g_flush_owner
// 写成自己；ISR 里读这个标记把"完成事件"路由回真正的发起方。
//
// 默认值 GFX 保证"从未切换过"的老路径（只有 gfx 在刷）行为与改动前一致。
namespace {
constexpr uint8_t kFlushOwnerGfx = 0;
constexpr uint8_t kFlushOwnerLvgl = 1;
constexpr uint8_t kFlushOwnerNone = 2;
// 第三种归属：无人认领。
//
// 这是为了修一个真实的 panic：
//   gfx 引擎被销毁（gfx_emote_deinit 释放上下文）后，panel_io 的
//   on_color_trans_done 槽里仍然挂着旧引擎的 user_ctx 指针 —— 这个槽只有
//   「重新注册」才能改，而重新注册要等新引擎构造完成。于是存在一个窗口：
//   旧引擎已释放、新引擎未登记，此时任何一笔在途传输完成都会走进本函数，
//   把野 user_ctx 当 gfx 句柄去 gfx_emote_flush_ready => LoadProhibited。
//   另一个更容易中招的时序：SwitchToGfx 先把 owner 改回 GFX，可 LVGL 还有
//   一笔 DMA 没完成，它完成后同样会被误判成 gfx 的传输。
// 因此销毁引擎前把 owner 置成 NONE，本函数看到 NONE 就只做丢弃，
// 不碰任何句柄；等新引擎构造完（回调已重新登记）再改回 GFX。
std::atomic<uint8_t> g_flush_owner{kFlushOwnerGfx};
std::atomic<emote::LvglFlushDoneFn> g_lvgl_flush_done{nullptr};

// 在途 SPI 传输计数 + "全部结束"信号量。
//
// 为什么光置 NONE 不够、必须等传输真正结束：
//   gfx_emote_flush_ready() 在 ISR 里走 xEventGroupSetBitsFromISR()，
//   它把"置位"动作打包成一条命令投递给 FreeRTOS 定时器任务，稍后才执行。
//   于是引擎销毁后，定时器任务里仍可能残留针对 ctx->sync.event_group 的
//   命令 —— 而该 event group 已被 gfx_emote_deinit() 删除，执行时就是野指针。
//   实测崩溃栈正是：
//     prvTimerTask -> vEventGroupSetBitsCallback -> xEventGroupSetBits
//     -> spinlock_acquire -> LoadProhibited (EXCVADDR 0x1130)
//   所以销毁引擎前必须等到在途传输计数归零、命令全部投递完毕。
std::atomic<int> g_spi_inflight{0};
SemaphoreHandle_t g_spi_idle_sem{nullptr};

// 定时器队列屏障。
//
// 光等 SPI 传输结束**不够**：xEventGroupSetBitsFromISR() 只是把"置位"
// 打包成命令塞进 FreeRTOS 定时器队列，命令真正执行可能晚于传输完成中断，
// 也晚于我们看到的计数归零。等 g_spi_inflight 变 0 只说明"总线空了"，
// 不等于"队列里没有指向 ctx->sync.event_group 的命令了"。
//
// 定时器队列是 FIFO，所以只要往同一个队列里放一个我们自己的命令并等它
// 执行完，就能保证它之前的命令都已处理完。这里用一次性 esp_timer 实现，
// 因为它的回调同样跑在 FreeRTOS 定时器任务里（ESP_TIMER_TASK）。
SemaphoreHandle_t g_timer_barrier_sem{nullptr};
esp_timer_handle_t g_timer_barrier{nullptr};

void TimerBarrierCb(void* arg)
{
    (void)arg;
    const SemaphoreHandle_t sem = g_timer_barrier_sem;
    if (sem != nullptr) {
        xSemaphoreGive(sem);
    }
}

// 投递一个屏障进定时器队列并阻塞到它执行（即此前所有命令都已执行）。
// 返回等待实际耗时(us)，超时返回 -1。
int64_t WaitTimerQueueDrain(const int timeout_ms)
{
    if (g_timer_barrier_sem == nullptr) {
        g_timer_barrier_sem = xSemaphoreCreateBinary();
        if (g_timer_barrier_sem == nullptr) {
            return -1;
        }
    }
    if (g_timer_barrier == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &TimerBarrierCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,   // 必须走定时器任务，不能用 ISR
            .name = "gfx_drain",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&args, &g_timer_barrier) != ESP_OK) {
            g_timer_barrier = nullptr;
            return -1;
        }
    }
    // 先清掉可能遗留的信号，否则下面会立刻返回、根本没等
    xSemaphoreTake(g_timer_barrier_sem, 0);
    const int64_t t0 = esp_timer_get_time();
    if (esp_timer_start_once(g_timer_barrier, 1000) != ESP_OK) {
        return -1;
    }
    const BaseType_t ok = xSemaphoreTake(g_timer_barrier_sem, pdMS_TO_TICKS(timeout_ms));
    const int64_t elapsed = esp_timer_get_time() - t0;
    if (ok != pdTRUE) {
        return -1;
    }
    return elapsed;
}
}  // namespace

// ----------------------------------------------------------------------------
// panel_io 完成回调的归属仲裁接口
// ----------------------------------------------------------------------------
// 这三个函数都很短且会被 ISR/渲染任务高频调用，因此只做原子读写，不加锁。
// 注意：本文件整体处于 namespace emote 之内，函数定义不能再写 emote:: 前缀。
void GfxSetFlushOwnerLvgl()
{
    g_flush_owner.store(kFlushOwnerLvgl, std::memory_order_relaxed);
}

void GfxSetFlushOwnerGfx()
{
    g_flush_owner.store(kFlushOwnerGfx, std::memory_order_relaxed);
}

void GfxSetFlushOwnerNone()
{
    g_flush_owner.store(kFlushOwnerNone, std::memory_order_relaxed);
}

bool GfxFlushOwnerIsNone()
{
    return g_flush_owner.load(std::memory_order_relaxed) == kFlushOwnerNone;
}

void GfxSpiInflightAdd()
{
    g_spi_inflight.fetch_add(1, std::memory_order_acq_rel);
}

// 归零时唤醒等待方。isr=true 时必须用 FromISR 版本。
static void SpiInflightRelease(const bool isr)
{
    const SemaphoreHandle_t sem = g_spi_idle_sem;
    if (sem == nullptr) {
        return;
    }
    if (isr) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(sem, &woken);
        if (woken) {
            portYIELD_FROM_ISR();
        }
    } else {
        xSemaphoreGive(sem);
    }
}

void GfxSpiInflightDone()
{
    if (g_spi_inflight.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
        SpiInflightRelease(false);
    }
}

// 是否有在途 SPI 传输。供 PageManager 在"交给另一边之前"做屏障用。
// 用 acquire 语义：调用方看到 false 之后，之前的所有 DMA 完成回调
// （它们分别对各自的缓冲做了写操作）都已完成，不会再碰面板。
bool GfxSpiIdle()
{
    return g_spi_inflight.load(std::memory_order_acquire) <= 0;
}

void GfxSetLvglFlushDoneHandler(const LvglFlushDoneFn fn)
{
    g_lvgl_flush_done.store(fn, std::memory_order_relaxed);
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
    //
    // 后端分时复用：这块 panel_io 的完成回调槽位永远属于 gfx，LVGL 的传输
    // 完成也走这里。因此先看"这次传输是谁发起的"，再路由到对应的消费方。
    const uint8_t owner = g_flush_owner.load(std::memory_order_relaxed);
    // 无论这笔传输最终归属谁，它都已经离开总线了，先记账归零。
    // 归零后释放 g_spi_idle_sem，销毁路径靠它判断"总线真的空了"。
    if (g_spi_inflight.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
        SpiInflightRelease(true);
    }
    if (owner == kFlushOwnerNone) {
        // 无人认领：引擎正在销毁/重建的窗口期，user_ctx 可能是已释放的旧句柄，
        // 绝不能解引用。丢弃这一笔完成事件即可 —— 发起方（LVGL 或旧 gfx）
        // 此时都已经不再等待它。
        return true;
    }
    if (owner == kFlushOwnerLvgl) {
        // 发起方是 LVGL：把完成事件交给 LVGL 的 flush_ready。
        // 绝不能再调 gfx_emote_flush_ready —— 那不是 gfx 的传输，
        // 会让 gfx 提前复用仍在被 DMA 读取的缓冲（画面折叠）。
        const emote::LvglFlushDoneFn done = g_lvgl_flush_done.load(std::memory_order_relaxed);
        if (done != nullptr) {
            done();
        }
        return true;
    }

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
        // 【极其关键】必须尊重 NONE：销毁引擎期间 owner 被上层置成 NONE，
        // 而 gfx_core_task 退出前还会完整跑一轮 gfx_refr_handler → OnFlush。
        // 如果这里无条件把 owner 写回 GFX，那一轮就会发起一笔新 DMA，
        // 而 gfx_emote_deinit() 紧接着就 vEventGroupDelete(ctx->sync.event_group)。
        // 这笔 DMA 完成时 ISR 看到 owner=GFX，会调 gfx_emote_flush_ready(ctx已被释放)
        // → xEventGroupSetBitsFromISR(已删除的 event group) → 定时器任务里野指针崩溃。
        // 所以 NONE 期间绝不能发新传输，也绝不能改写 owner。
        if (g_flush_owner.load(std::memory_order_relaxed) == kFlushOwnerNone) {
            return;
        }
        // 仅发起传输；完成通知交由 OnFlushIoReady（on_color_trans_done）负责。
        // 发起前声明"这笔传输归 gfx"，供 ISR 路由使用。标记必须写在
        // draw_bitmap 之前：若写在之后，DMA 可能已经完成并进中断，
        // 中断里就会读到上一笔遗留的 LVGL 标记。
        g_flush_owner.store(kFlushOwnerGfx, std::memory_order_relaxed);
        // 记账：这笔传输对外可见之前先加一，避免销毁路径看到"已归零"
        // 却在下一瞬又冒出一笔新的 DMA。
        g_spi_inflight.fetch_add(1, std::memory_order_acq_rel);
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
    // 记下面板句柄与分辨率：引擎在进出桌面时会销毁/重建，
    // 重建时需要用它们重新 new EmoteEngine。
    panel_ = panel;
    width_ = width;
    height_ = height;
    // 引擎生命周期锁：保护 engine_ 的销毁/重建与所有使用者之间的互斥。
    // 必须在任何 DisplayLockGuard 使用之前创建。
    engine_mutex_ = xSemaphoreCreateRecursiveMutex();
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
    // 3.5 桌面激活：表情静默，只记录目标表情，不碰 gfx 对象（不产生脏标记）
    if (g_desktop_active) {
        g_last_emotion = emotion;
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

    DisplayLockGuard lock(this);
    engine_->SetEyes(emotion, repeat, fps, this);
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    if (!engine_) {
        return;
    }
    // 桌面激活：不把对话气泡写进 gfx（否则会叠在桌面上）
    if (g_desktop_active) {
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

    // 桌面激活：状态变化不写进 gfx（否则底部状态条会残留在桌面上）。
    // 注意：这里【只】跳过渲染，App / 音频侧的状态机不受影响。
    if (g_desktop_active) {
        return;
    }

    DisplayLockGuard lock(this);

    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_LISTENING, this);
        engine_->SetEyes("happy", true, 20, this);
        engine_->SetIcon(ICON_MIC, this);
    } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
        engine_->SetIcon(ICON_BATTERY, this);
        // Default expression page: emoji eyes + small time label.
        SetUIDisplayMode(UIDisplayMode::SHOW_TIME, this);
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

    // 桌面激活：通知不写进 gfx（会盖在桌面上）。保留日志便于排查。
    if (g_desktop_active) {
        return;
    }

    DisplayLockGuard lock(this);
    gfx_label_set_text(g_obj_label_toast, notification);
    SetUIDisplayMode(UIDisplayMode::SHOW_TIPS, this);
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    if (!engine_) {
        return;
    }

    // 桌面激活：表情页整屏不可见，这里每秒的时间/电量刷新既看不见，
    // 又会标记 label dirty 让 gfx 持续整屏重绘，必须直接让路。
    // 电量仍在下面的 BatteryLevel 钩子里按需读取（Timer 应用要用）。
    if (g_desktop_active) {
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
                g_battery_pct = level;
            } else {
                // 读不到（未接电池/芯片掉线）→ 置未知，电量百分比显示 "--"
                g_battery_pct = -1;
            }
        }
    }

    // Only display time when battery icon is shown (idle state)
    DisplayLockGuard lock(this);
    if (g_current_icon_type == ICON_BATTERY) {
        // Legacy small time label fallback.
        time_t now;
        struct tm timeinfo;
        time(&now);

        setenv("TZ", "GMT+0", 1);
        tzset();
        localtime_r(&now, &timeinfo);

        // 去重：小时间标签 "HH:MM" 在同一分钟内文本不变，每秒都 set_text 会标记
        // label dirty 触发引擎整屏重绘（双缓冲异步下整屏偏移/闪）。仅在分钟变化时更新。
        // 变量提到文件作用域：引擎重建后必须由 InvalidateDedupCache() 清空，
        // 否则新的空 label 永远不会被写入文本。
        if (timeinfo.tm_min != g_status_last_min) {
            g_status_last_min = timeinfo.tm_min;
            char time_str[6];
            snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
            gfx_label_set_text(g_obj_label_clock, time_str);
        }

        // 电量百分比：与图标同一时刻、同一位置更新。
        // 去重：只在数值真正变化时 set_text，避免每秒标脏触发整屏重绘。
        // 同样提到文件作用域，理由同上。
        if (g_battery_pct != g_status_last_pct) {
            g_status_last_pct = g_battery_pct;
            char pct_str[8];
            if (g_battery_pct < 0) {
                snprintf(pct_str, sizeof(pct_str), "--");
            } else {
                // 注意：printf 的 "%.3d" 精度表示【最少】位数，不是上限，
                // 对 int 仍可能输出 10 位，GCC 的 -Wformat-truncation 据此报错。
                // 这里回收成 uint8_t（最大 255，最多 3 位），
                // 让编译器能从类型上算出上限，才是真正消除告警的写法。
                const uint8_t pct = static_cast<uint8_t>(
                    g_battery_pct > 100 ? 100 : g_battery_pct);
                snprintf(pct_str, sizeof(pct_str), "%u%%", static_cast<unsigned>(pct));
            }
            gfx_label_set_text(g_obj_label_battery, pct_str);
        }

        SetUIDisplayMode(UIDisplayMode::SHOW_TIME, this);
    }
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
    // 引擎可能因进入桌面而被销毁（见 SetDesktopActive），此时只登记资源，
    // 等回到表情页重建引擎后由 SetDesktopActive 统一重放。
    if (name == "happy" && engine_) {
        engine_->SetEyes("happy", loop, fps > 0 ? fps : 20, this);
    }
}

void EmoteDisplay::AddIconData(const std::string &name, const void* const data, const size_t size)
{
    icon_data_map_[name] = AssetData(data, size);
    ESP_LOGD(TAG, "Added icon data: %s, size: %d", name.c_str(), size);

    DisplayLockGuard lock(this);
    if (name == ICON_WIFI_FAILED && engine_) {
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
        {g_obj_img_status,     UI_ELEMENT_STATUS_ICON}
    };

    DisplayLockGuard lock(this);

    // 记下来。启动时 assets.cc 只调一次，而桌面切换会重建整个引擎、
    // 对象全部换新；不缓存的话重建后就没法把 layout.json 的配置补回去。
    // 同名记录只保留最后一次（与"后写覆盖先写"的语义一致）。
    bool recorded = false;
    for (auto &rec : layouts_) {
        if (rec.name == name) {
            rec = LayoutRecord{name, align_str, x, y, width, height};
            recorded = true;
            break;
        }
    }
    if (!recorded) {
        layouts_.push_back(LayoutRecord{name, align_str, x, y, width, height});
    }

    for (const auto &element : elements) {
        if (name == element.name && element.obj) {
            gfx_obj_align(element.obj, align_enum, x, y);
            if (width > 0 && height > 0) {
                gfx_obj_set_size(element.obj, width, height);
            }
            return;
        }
    }

    // 找不到对象不算错误：可能是启动早期布局先于对象注册，
    // 也可能该元素确实不存在。记录保留着，重建后 ReplayLayouts 会再试一次。
    ESP_LOGD(TAG, "AddLayoutData: UI element '%s' not found (recorded for replay)", name.c_str());
}

// ----------------------------------------------------------------------------
// 引擎重建后重放 layout.json 的布局
// ----------------------------------------------------------------------------
// SetDesktopActive(false) 重建引擎后调用。调用方必须已持有显示锁
// （SetDesktopActive 里已经在 DisplayLockGuard 内），这里不再重复加锁，
// 否则递归锁虽然能过，但会让锁的持有关系变得难读。
void EmoteDisplay::ReplayLayouts()
{
    if (layouts_.empty() || engine_ == nullptr) {
        return;
    }

    struct UIElement {
        gfx_obj_t* obj;
        const char* name;
    };
    const UIElement elements[] = {
        {g_obj_anim_eye,       UI_ELEMENT_EYE_ANIM},
        {g_obj_label_toast,    UI_ELEMENT_TOAST_LABEL},
        {g_obj_label_clock,    UI_ELEMENT_CLOCK_LABEL},
        {g_obj_anim_listen,    UI_ELEMENT_LISTEN_ANIM},
        {g_obj_img_status,     UI_ELEMENT_STATUS_ICON}
    };

    int applied = 0;
    for (const auto &rec : layouts_) {
        for (const auto &element : elements) {
            if (rec.name == element.name && element.obj) {
                const char align_enum = StringToGfxAlign(rec.align);
                gfx_obj_align(element.obj, align_enum, rec.x, rec.y);
                if (rec.width > 0 && rec.height > 0) {
                    gfx_obj_set_size(element.obj, rec.width, rec.height);
                }
                applied++;
                break;
            }
        }
    }
    ESP_LOGW(TAG, "layouts replayed after engine rebuild: %d/%d",
             applied, static_cast<int>(layouts_.size()));
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
    // 这里要在两层上互斥，缺一不可：
    //
    // 1) engine_mutex_：保护 engine_ 这个指针本身的生命周期。
    //    SetDesktopActive 会在别的任务里 engine_.reset()，若只用 gfx 自己的锁，
    //    会出现 A 任务拿到旧引擎句柄 -> B 任务释放引擎 -> A 任务解锁已释放内存
    //    的 use-after-free。engine_mutex_ 与引擎生命周期无关，所以安全。
    //
    // 2) gfx_emote_lock：gfx_core 的"渲染互斥锁"，用来阻止 gfx_core 线程
    //    在我们改对象树（set_src / anim_start 等）时并发渲染。少了它，
    //    对象树会在渲染过程中被修改，warn "Update current object failed" 甚至崩溃。
    //
    // 顺序固定：先 engine_mutex_ 再 gfx 锁。SetDesktopActive 释放引擎时只拿
    // engine_mutex_，它天然会等所有持锁者离开，所以不会形成环。
    if (!engine_mutex_) {
        return false;
    }
    if (xSemaphoreTakeRecursive(engine_mutex_,
                                timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY) != pdTRUE) {
        return false;
    }
    // 拿到 engine_mutex_ 后引擎是稳定的，这里的句柄不会在下面被释放。
    // gfx_per_engine_ 记录当前持锁嵌套层里真正拿到 gfx 锁的次数，
    // 用计数而不是 bool，否则递归嵌套时内层 Unlock 会把外层的标记清掉
    // （内层把 gfx_locked_ 置 false，外层 Unlock 就漏掉一次 gfx_emote_unlock）。
    if (engine_ && engine_->GetEngineHandle()) {
        if (gfx_emote_lock(engine_->GetEngineHandle()) == ESP_OK) {
            ++gfx_lock_depth_;
        }
    }
    return true;
}

void EmoteDisplay::Unlock()
{
    if (gfx_lock_depth_ > 0 && engine_ && engine_->GetEngineHandle()) {
        --gfx_lock_depth_;
        gfx_emote_unlock(engine_->GetEngineHandle());
    }
    if (engine_mutex_) {
        xSemaphoreGiveRecursive(engine_mutex_);
    }
}

// ============================================================================
// 桌面占屏 ↔ 表情页 切换
// ============================================================================
// ----------------------------------------------------------------------------
// 引擎独占式切换（方案 B）
// ----------------------------------------------------------------------------
// 设计要点：gfx 与 LVGL 在同一时刻只允许存在一个。
//
// 之前所有"分时共存"的尝试都失败了，根因是 esp_lcd_panel_draw_bitmap 并非
// 原子操作（见 esp_lcd_st77916.c:472-501）：
//   tx_param(CASET) -> tx_param(RASET) -> tx_color(排队)
// 前两步是同步轮询、要独占 SPI 总线；第三步是异步排队后立刻返回。
// 两个后端各自在不同核心、不同任务里调用它，中间没有任何总线归属协议，
// 撞车就是：
//   E spi_master: Cannot acquire bus when a polling transaction is in progress
//   E st77916: panel_st77916_draw_bitmap(484): send command failed
// 而且 trans_queue_depth = 10，排队中的像素对另一方完全不可见，加锁也防不住。
//
// 因此这里改为"轮流独占"：进桌面销毁 gfx 引擎，回表情重建。
// gfx_emote_deinit() 是同步的（gfx_core.c:439-440 发 NEEDED_DELETE 后死等
// DELETE_DONE），返回时 gfx_core 线程一定已退出、帧缓冲已释放，
// 所以销毁之后不可能再有任何 gfx 发起的 SPI 传输。
//
// 重建之所以安全：
//   - EmoteEngine 构造函数完整做了 InitializeGraphics + SetupUI + RegisterCallbacks；
//   - gfx_image_decoder 可重入（gfx_img_dec.c:273-283 deinit 会清零 decoder_count）；
//   - 6 个 g_obj_* 是文件级 static，SetupUI 会重新赋值，无需手工清理；
//   - emoji_data_map_ / icon_data_map_ 存在 EmoteDisplay 上，不随引擎销毁而丢失。
// ----------------------------------------------------------------------------
void EmoteDisplay::SetDesktopActive(const bool active)
{
    if (active) {
        if (g_desktop_active) {
            return;
        }
        g_desktop_active = true;  // 先置位，让所有渲染入口立刻失效

        // 销毁引擎。析构函数里 gfx_emote_deinit 会同步等 gfx_core 线程退出。
        // 必须持 engine_mutex_：别的任务可能正暂停在 DisplayLockGuard 里
        // 使用引擎，等它退出临界区后我们才能真正释放。
        if (engine_mutex_) {
            xSemaphoreTakeRecursive(engine_mutex_, portMAX_DELAY);
        }
        // 关键：销毁前先把 panel_io 完成回调的归属清成 NONE。
        // 否则 gfx_emote_deinit 释放上下文后，槽里挂着的 user_ctx 就成了野指针，
        // 任何一笔在途传输的完成 ISR 都会拿它去 gfx_emote_flush_ready => panic。
        GfxSetFlushOwnerNone();
        // 再停掉所有动画，让 gfx_core 不再产生新的 flush 请求，
        // 尽量缩小"销毁过程中仍有在途传输"的窗口。
        if (engine_ && engine_->GetEngineHandle()) {
            gfx_anim_stop(g_obj_anim_eye);
            gfx_anim_stop(g_obj_anim_listen);
        }
        // 关键二：等 SPI 总线真正空闲后再销毁。
        // 只置 NONE 只能挡住"之后的"完成事件，挡不住已经投递出去的
        // gfx_emote_flush_ready 命令 —— 那些命令在定时器任务队列里等着执行，
        // 而它们的目标 event group 会在 engine_.reset() 里被删除，
        // 执行时就是野指针（历史崩溃栈：
        //   prvTimerTask -> vEventGroupSetBitsCallback -> xEventGroupSetBits
        //   -> LoadProhibited，EXCVADDR 0x1130）。
        //
        // 这里阻塞到 g_spi_inflight 归零：FPGA/SPI 一笔 360x40 的 DMA
        // 只有几毫秒，超时给 200ms 纯粹是兜底（真超时也继续往下走，
        // 但不能让 touch_task 卡死在这里影响语音链路）。
        if (g_spi_idle_sem == nullptr) {
            g_spi_idle_sem = xSemaphoreCreateBinary();
        }
        if (g_spi_idle_sem != nullptr) {
            constexpr int kDrainTimeoutMs = 200;
            // 先吃掉可能遗留的旧信号（上一次归零时留下的），
            // 否则下面会立刻返回、根本没等。
            xSemaphoreTake(g_spi_idle_sem, 0);
            int64_t t_drain = esp_timer_get_time();
            while (g_spi_inflight.load(std::memory_order_acquire) > 0) {
                if (xSemaphoreTake(g_spi_idle_sem,
                                   pdMS_TO_TICKS(kDrainTimeoutMs)) != pdTRUE) {
                    ESP_LOGW(TAG, "SPI drain timeout: inflight=%d",
                             g_spi_inflight.load(std::memory_order_relaxed));
                    break;
                }
            }
            const int drain_us = static_cast<int>(esp_timer_get_time() - t_drain);
            if (drain_us > 2000) {
                ESP_LOGW(TAG, "SPI drained in %d us", drain_us);
            }
        }
        // 关键三：排空 FreeRTOS 定时器队列里残留的命令。
        //
        // SPI 传输结束只保证总线空了，不保证 gfx_emote_flush_ready() 投递到
        // 定时器队列的 vEventGroupSetBitsCallback 已经执行。那条命令的目标是
        // ctx->sync.event_group，而它会在 engine_.reset() 里被删除。
        // 定时器队列 FIFO，因此投递一个自己的屏障并等它执行，就能保证
        // 之前的命令全部处理完毕。
        const int64_t barrier_us = WaitTimerQueueDrain(300);
        if (barrier_us < 0) {
            ESP_LOGW(TAG, "timer queue drain timeout, proceeding anyway");
        } else if (barrier_us > 2000) {
            ESP_LOGW(TAG, "timer queue drained in %d us", static_cast<int>(barrier_us));
        }
        // 关键四：先自己删掉 UI 对象，堵住 gfx 组件的对象泄漏。
        // gfx_emote_deinit() 只 free 链表节点、不 free 对象本身，
        // 不在这里删就会每切换一次泄漏 6 个对象，几次之后
        // gfx_emote_init 分配帧缓冲失败、表情页彻底死掉。
        //
        // 必须持 gfx 渲染锁：gfx_obj_delete 会改 ctx->disp.child_list，
        // 而 gfx_core_task 每轮都在 gfx_refr_handler 里遍历同一个链表。
        // engine_mutex_ 只保护我们自己这边对 engine_ 的使用，挡不住它。
        if (engine_ && engine_->GetEngineHandle() &&
            gfx_emote_lock(engine_->GetEngineHandle()) == ESP_OK) {
            TeardownUI();
            gfx_emote_unlock(engine_->GetEngineHandle());
        } else {
            // 拿不到锁也要删，否则泄漏照样发生；这里只是失去了"遍历不冲突"的保证。
            ESP_LOGW(TAG, "TeardownUI: gfx lock failed, deleting anyway");
            TeardownUI();
        }
        engine_.reset();
        EmoteEngine::InvalidateDedupCache();
        if (engine_mutex_) {
            xSemaphoreGiveRecursive(engine_mutex_);
        }

        ESP_LOGW(TAG, "desktop active: gfx engine destroyed, SPI owned by LVGL");
        return;
    }

    if (!g_desktop_active) {
        return;
    }
    g_desktop_active = false;

    // 重建引擎。构造完成时 SetupUI 已把 6 个 g_obj_* 重新指向新对象，
    // RegisterCallbacks 也已把 panel_io 的完成回调重新登记给新引擎。
    // 整个重建过程持 engine_mutex_，避免别的任务看到"半初始化"的引擎。
    const int64_t t0 = esp_timer_get_time();
    {
        if (engine_mutex_) {
            xSemaphoreTakeRecursive(engine_mutex_, portMAX_DELAY);
        }
        if (panel_ && width_ > 0 && height_ > 0) {
            engine_ = std::make_unique<EmoteEngine>(panel_, panel_io_, width_, height_, this);
        }
        if (engine_mutex_) {
            xSemaphoreGiveRecursive(engine_mutex_);
        }
    }
    // 注意：即使 gfx_emote_init 内部失败（比如帧缓冲分配不出来），
    // EmoteEngine 对象本身仍会被构造成功，只是 engine_handle_ 为空。
    // 所以不能只判 !engine_，必须判句柄，否则会带着一个空壳引擎继续跑，
    // 之后每秒刷 60 行 "engine_handle_ is nullptr"，画面永久卡死。
    if (!engine_ || engine_->GetEngineHandle() == nullptr) {
        ESP_LOGE(TAG, "desktop inactive: cannot rebuild engine (panel=%p, %dx%d, handle=%p)",
                 static_cast<void*>(panel_), width_, height_,
                 engine_ ? engine_->GetEngineHandle() : nullptr);
        // 把空壳释放掉，让后续调用走 engine_ == nullptr 的早退分支，
        // 而不是反复对一个无句柄的对象发起渲染调用。
        engine_.reset();
        return;
    }
    // 新引擎构造完成意味着 RegisterCallbacks 已把正确的 user_ctx 写回槽位，
    // 此时才可以恢复"归 gfx 所有"。
    // 位置要点：必须在下面对 SetEyes/SetIcon 之前恢复 —— 它们会启动动画，
    // gfx_core 随即发起 flush，而 OnFlush 在 NONE 期间会直接丢弃，
    // 恢复晚了会让重建后的第一帧白刷。
    GfxSetFlushOwnerGfx();
    const int64_t rebuild_us = esp_timer_get_time() - t0;

    // 重建后去重缓存必须清零：g_eyes_applied 等仍是旧值，
    // 不清零的话 SetEyes 会判"表情没变"而跳过 set_src，画面留空。
    EmoteEngine::InvalidateDedupCache();

    // 强制回写一次完整画面：表情 + 图标 + 状态条。
    {
        DisplayLockGuard lock(this);

        // 先重放 layout.json 的布局。
        // 必须排在最前面：它会改对象的 align 和尺寸，
        // 而下面的 SetIcon / UpdateStatusBar 都是往"已经摆好的框"里写内容。
        // 不重放的话，重建后用的是 SetupUI 里的兜底常量，
        // 与 layout.json 的实际配置不一致 —— 电池图标会跳到左上角。
        ReplayLayouts();

        // 表情：必须按这个表情自己的 loop/fps 恢复，不能写死 repeat=false。
        // 写死 false 会让循环类表情（idle/neutral 之外的大部分）播完一遍就停，
        // 屏幕上留下的是"某一帧静止画面"，看上去就是表情错位/残缺。
        // 这里复刻 SetEmotion 里同一套取值规则，保证恢复出来的状态与
        // 首次进入表情页时完全一致。
        {
            const AssetData emoji_data = GetEmojiData(g_last_emotion);
            bool repeat = emoji_data.loop;
            int fps = emoji_data.fps > 0 ? emoji_data.fps : 10;
            if (std::strcmp(g_last_emotion.c_str(), "idle") == 0 ||
                std::strcmp(g_last_emotion.c_str(), "neutral") == 0) {
                repeat = false;
            }
            engine_->SetEyes(g_last_emotion, repeat, fps, this);
        }
        if (!g_current_icon_type.empty()) {
            engine_->SetIcon(g_current_icon_type, this);
        }
        // SetUIDisplayMode 按模式去重，桌面期间模式缓存还停在旧值，
        // 打成 SHOW_LISTENING 这个"初值"逼它真跑一次。
        g_current_mode = UIDisplayMode::SHOW_LISTENING;
        UpdateStatusBar(true);
    }

    ESP_LOGW(TAG, "desktop inactive: gfx engine rebuilt in %d us",
             static_cast<int>(rebuild_us));
}

int EmoteDisplay::GetBatteryPercent()
{
    // 与 UpdateStatusBar 共用一个 5 秒节流窗口，避免两处各读一次 I2C。
    static int64_t s_last_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_us >= 5 * 1000 * 1000) {
        s_last_us = now_us;
        int level = -1;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging) && level >= 0) {
            g_battery_pct = level;
        } else {
            g_battery_pct = -1;
        }
    }
    return g_battery_pct;
}

} // namespace emote
