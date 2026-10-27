// ============================================================================
// emote_app.cc — 把 gfx 表情界面变成 LVGL 桌面里的"表情应用"
// ----------------------------------------------------------------------------
// 关键事实（全部在本仓库组件源码中核实过，不是推测）
// ----------------------------------------------------------------------------
// 1) panel 完成回调是覆盖式注册
//    esp_lcd_panel_io_register_event_callbacks() 内部直接覆盖 cb + user_ctx。
//    gfx 与 esp_lvgl_port 都要 on_color_trans_done，所以同一时刻只能有一个
//    引擎往屏上刷。
//
// 2) 隐藏对象挡不住 gfx 刷屏
//    gfx_core.c:124  gfx_object_handler() 结尾是【无条件】 updated = true。
//    只要对象树非空，gfx 每轮都整屏重绘并调用 flush_cb。
//    → 停 gfx 不能靠 gfx_obj_set_visible，必须换掉 flush_cb。
//
// 3) 换 flush_cb 时不能"什么都不做"
//    gfx_refr_handler() 每块传输后等 WAIT_FLUSH_DONE，上限 50ms
//    （gfx_core.c:687）。若 flush 里不发数据也不回完成，23 块 × 50ms
//    ≈ 1.15s/帧，会长时间占着 SPI。
//    → 用"空 flush + 立即 flush_ready"，gfx 只在内部空转（1ms 级），
//      不产生任何 SPI 传输，LVGL 独占总线。
//
// 4) 切换必须在 gfx 的 lock_mutex 内完成
//    gfx_core_task 整个循环持 lock_mutex。持锁改 flush 覆盖 + 重注册 panel
//    回调，可保证不会在传输中途切换。
//
// 5) LVGL 侧的 flush 也要接管
//    lvgl_port_add_disp() 内部会把 on_color_trans_done 注册为 LVGL 的回调，
//    user_ctx = lv_display_t*；但它的 flush_cb（lvgl_port_flush_callback）是
//    static，外部拿不到，无法保存/恢复。因此 InitBackend 里直接把 s_lvdisp 的
//    flush_cb 换成我们自己的 EmoteAppLvglFlushReal（SPI 路径等价实现）。
//    gfx 激活时再把它切成 EmoteAppLvglFlushNop（只通知 LVGL 完成、不发 SPI），
//    切回 LVGL 时恢复 Real。这样同一时刻只有一个驱动往 panel 发数据。
// 6) 改 flush_cb 不需要 gfx 的锁，但换 on_color_trans_done 必须在 gfx 的
//    lock 内（见 ActivateGfx/ActivateLvgl），与上面第 4 点一致。
// ============================================================================
#include "emote_app.h"

#if CONFIG_EMOTE_AS_LVGL_APP

#include <esp_log.h>
#include <lvgl.h>
#include <esp_lcd_panel_io.h>   // esp_lcd_panel_draw_bitmap
#include <esp_lvgl_port.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "board.h"
#include "app_launcher.h"
#include "clock_page_lvgl.h"
#include "gfx.h"

#define TAG "EmoteApp"

static constexpr int kScreenW = 360;
static constexpr int kScreenH = 360;
static constexpr int kSwipeDiv = 6;     // 位移阈值 = 屏宽 / 6
static constexpr int kSwipeMinPx = 45;  // 阈值下限

namespace {

struct AppPage {
    lv_obj_t* obj = nullptr;
    const char* name = "page";
    void (*on_enter)() = nullptr;
    void (*on_leave)() = nullptr;
};

std::vector<AppPage> s_pages;
int s_cur_page = 0;
constexpr int kEmotePage = 0;   // 表情页固定是第 0 页

emote::EmoteDisplay* s_emote = nullptr;
gfx_handle_t s_gfx = nullptr;
lv_display_t* s_lvdisp = nullptr;
esp_lcd_panel_io_handle_t s_panel_io = nullptr;
esp_lcd_panel_handle_t    s_panel = nullptr;   // 供 LVGL Real flush 使用

bool s_gfx_active = false;
bool launcher_created_ = false;

lv_obj_t* s_emote_overlay = nullptr;   // 表情页上由 LVGL 绘制的角标层
lv_obj_t* s_time_label = nullptr;

bool s_pressing = false;
int s_down_x = 0;
int s_down_y = 0;
int s_last_x = 0;
int s_last_y = 0;

// ---------------------------------------------------------------------------
// panel 完成回调
// ---------------------------------------------------------------------------
bool GfxTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx)
{
    // 等价于 EmoteEngine::OnFlushIoReady
    return gfx_emote_flush_ready(static_cast<gfx_handle_t>(ctx), false);
}

bool LvglTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx)
{
    // 等价于 esp_lvgl_port 的 lvgl_port_flush_io_ready_callback
    // 注意：必须用 LVGL 9 的规范名 lv_display_flush_ready()。
    // esp_lvgl_port 里写的 lv_disp_flush_ready() 只是 lv_api_map_v8.h 的兼容宏，
    // 依赖它会让本文件在未包含该兼容头的编译单元里报错。
    lv_display_flush_ready(static_cast<lv_display_t*>(ctx));
    return false;
}

}  // namespace

// 图像描述符定义在全局命名空间（见 display/images/*.c），这里在
// emote_app 命名空间之外声明，避免被加上 emote_app:: 前缀导致链接失败。
LV_IMAGE_DECLARE(img_app_timer_64_64);
LV_IMAGE_DECLARE(img_app_settings_64_64);

// gfx 在"挂起"状态下的 flush：不发数据，立即回完成，让 gfx 循环空转。
// 必须是外部链接的普通 C 函数指针（写入 emote_display.cc 的转发表）。
static void EmoteAppGfxFlushSuspended(void* handle, int x1, int y1, int x2, int y2, const void* data)
{
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)data;
    gfx_emote_flush_ready(static_cast<gfx_handle_t>(handle), false);
}

// ---------------------------------------------------------------------------
// LVGL 侧的 flush：同样要区分"谁持有面板"
// ---------------------------------------------------------------------------
// 根因：esp_lvgl_port 的 taskLVGL 常驻运行，一旦 active screen 变脏就会通过
// lvgl_port_flush_callback -> esp_lcd_panel_draw_bitmap 往【同一个 panel】刷屏。
// emote_app 让 gfx 画表情页时，LVGL 仍在后台跑定时器（表情页的 1s 时间标签会
// 每秒置脏），于是两个驱动并发往同一 panel_io 发 SPI，画面被破坏 -> 黑屏。
//
// 修复（方案 2）：完全接管 s_lvdisp 的 flush_cb。
//   - gfx 激活时：换成 Nop，只通知 LVGL"刷完了"，【不发任何 SPI】，让 LVGL
//     后台任务空转，不碰总线；面板完全由 gfx 独占。
//   - LVGL 激活时：换成 Real，走真正的 SPI 传输（行为对齐 lvgl_port_flush_callback
//     的 SPI 分支）。
// 库的 lvgl_port_flush_callback 是 static，外部拿不到，因此这里自己实现等价版，
// 并在 InitBackend 里把库的 flush 替换成我们的 Real，使切换完全可控。
// panel 句柄直接用 InitBackend 保存的 s_panel，避免解析库内部结构体。

// 等价 lv_draw_sw_rgb565_swap：RGB565 高低字节交换（disp_cfg.flags.swap_bytes=1）。
// 不依赖 LVGL 内部头，直接实现避免脆弱的私有头 include。
static void Rgb565SwapBytes(uint8_t* buf, uint32_t px_count)
{
    for (uint32_t i = 0; i < px_count * 2; i += 2) {
        uint8_t t = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = t;
    }
}

static void EmoteAppLvglFlushNop(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    (void)area;
    (void)px_map;
    // 不发起任何 SPI 传输；仅告知 LVGL 该区域"已刷新"，否则 LVGL 会一直
    // 认为在等待刷新而反复重试/置脏。完成通知在 LvglTransDone 里不再需要，
    // 因为这里已经同步声明完成。
    lv_display_flush_ready(disp);
}

static void EmoteAppLvglFlushReal(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    // 等价 lvgl_port_flush_callback 的 SPI 分支（disp_type == SPI）：
    //   1) 若开启了 swap_bytes 则交换 565 字节序（disp_cfg.flags.swap_bytes = 1）
    //   2) esp_lcd_panel_draw_bitmap 发起传输
    //   3) 传输完成由已注册的 on_color_trans_done（LvglTransDone）回调负责
    //      lv_display_flush_ready，这里【不自调】。
    // panel 句柄直接用 InitBackend 保存的 s_panel，避免解析库内部结构体。
    (void)disp;
    if (px_map != nullptr && s_panel != nullptr) {
        Rgb565SwapBytes(px_map, lv_area_get_size(area));
        esp_lcd_panel_draw_bitmap(
            s_panel,
            area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    }
}

namespace emote_app {

// ===========================================================================
// 引擎切换
// ===========================================================================
void ActivateGfx()
{
    if (s_gfx == nullptr || s_gfx_active) {
        return;
    }
    gfx_emote_lock(s_gfx);          // 与 gfx_core_task 互斥，确保不在传输中途

    s_gfx_active = true;
    s_emote->SetGfxFlushOverride(nullptr);   // 恢复真实 flush
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = GfxTransDone };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, s_gfx));

    // 面板交给 gfx 期间，LVGL 后台任务不得往同一 panel 发 SPI（否则两驱动抢
    // 总线导致黑屏）。把 LVGL 的 flush 换成 no-op，只通知 LVGL 完成。
    lv_display_set_flush_cb(s_lvdisp, EmoteAppLvglFlushNop);

    gfx_emote_unlock(s_gfx);
    ESP_LOGI(TAG, "backend -> GFX");
}

void ActivateLvgl()
{
    if (s_gfx == nullptr || !s_gfx_active) {
        return;
    }
    gfx_emote_lock(s_gfx);

    s_gfx_active = false;
    // 先让 gfx 不再碰 SPI，再把面板交给 LVGL
    s_emote->SetGfxFlushOverride(EmoteAppGfxFlushSuspended);
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = LvglTransDone };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, s_lvdisp));

    // 面板交回 LVGL：恢复真正的 SPI flush
    lv_display_set_flush_cb(s_lvdisp, EmoteAppLvglFlushReal);

    gfx_emote_unlock(s_gfx);
    ESP_LOGI(TAG, "backend -> LVGL");
}

// ===========================================================================
// 页面管理
// ===========================================================================
void RegisterPage(lv_obj_t* page, const char* name, void (*on_enter)(), void (*on_leave)())
{
    if (page == nullptr) {
        return;
    }
    AppPage p;
    p.obj = page;
    p.name = name ? name : "page";
    p.on_enter = on_enter;
    p.on_leave = on_leave;
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    s_pages.push_back(p);
}

int PageCount()
{
    return static_cast<int>(s_pages.size());
}

bool IsEmotePageActive()
{
    return s_cur_page == kEmotePage;
}

void GotoPage(int index)
{
    if (index < 0 || index >= static_cast<int>(s_pages.size()) || index == s_cur_page) {
        return;
    }
    const int prev = s_cur_page;

    // ---------------- 去表情页 ----------------
    if (index == kEmotePage) {
        if (prev >= 0 && prev < static_cast<int>(s_pages.size()) && s_pages[prev].obj) {
            lv_obj_add_flag(s_pages[prev].obj, LV_OBJ_FLAG_HIDDEN);
        }
        s_cur_page = index;
        lv_screen_load(s_pages[index].obj);
        // 让 LVGL 把角标层刷出去（此刻 panel 回调仍是 LVGL 的）
        if (s_emote_overlay) {
            lv_obj_remove_flag(s_emote_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_emote_overlay);
        }
        lv_refr_now(s_lvdisp);
        // 面板交还 gfx
        ActivateGfx();
        ESP_LOGI(TAG, "page %d -> %d (emote)", prev, index);
        return;
    }

    // ---------------- 离开表情页 ----------------
    if (prev == kEmotePage) {
        ActivateLvgl();         // 先停 gfx
        if (s_emote_overlay) {
            lv_obj_add_flag(s_emote_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (prev >= 0 && prev < static_cast<int>(s_pages.size()) && s_pages[prev].obj) {
        lv_obj_add_flag(s_pages[prev].obj, LV_OBJ_FLAG_HIDDEN);
        if (s_pages[prev].on_leave) {
            s_pages[prev].on_leave();       // 例如：停掉时钟页的 1Hz 定时器
        }
    }
    s_cur_page = index;
    lv_screen_load(s_pages[index].obj);
    lv_obj_remove_flag(s_pages[index].obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_pages[index].obj);
    // 先让页面把自己的内容准备好（时钟页会在这里开表并补一帧），
    // 再统一刷新，避免"先闪一帧空白再出现内容"。
    if (s_pages[index].on_enter) {
        s_pages[index].on_enter();
    }
    lv_refr_now(s_lvdisp);
    ESP_LOGI(TAG, "page %d -> %d (%s)", prev, index, s_pages[index].name);
}

void NextPage()
{
    GotoPage(s_cur_page + 1);
}

void PrevPage()
{
    GotoPage(s_cur_page - 1);
}

// ===========================================================================
// 表情页
// ===========================================================================
lv_obj_t* CreateEmotePage(emote::EmoteDisplay* emote)
{
    s_emote = emote;
    s_gfx = static_cast<gfx_handle_t>(emote->GetEngineHandle());
    s_panel_io = emote->GetPanelIo();

    // 独立 screen：必须是 NULL 父对象，才能真正作为一层页面
    lv_obj_t* page = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    // 角标层：不可点击，保证"点屏唤起对话"仍由 Board/gfx 处理
    s_emote_overlay = lv_obj_create(page);
    lv_obj_remove_style_all(s_emote_overlay);
    lv_obj_set_size(s_emote_overlay, kScreenW, kScreenH);
    lv_obj_set_style_bg_opa(s_emote_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_emote_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_emote_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部时间（用 LVGL 内置 Montserrat，不额外占 flash）
    s_time_label = lv_label_create(s_emote_overlay);
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 4);

    lv_timer_create(
        [](lv_timer_t* t) {
            auto* label = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
            const time_t now = time(nullptr);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            if (tm_now.tm_year >= 2025 - 1900) {
                char buf[8];
                strftime(buf, sizeof(buf), "%H:%M", &tm_now);
                lv_label_set_text(label, buf);
            }
        },
        1000, s_time_label);

    AppPage p;
    p.obj = page;
    p.name = "emote";
    s_pages.insert(s_pages.begin(), p);   // 保证第 0 页
    return page;
}

// ===========================================================================
// 触摸 → 翻页
// ===========================================================================
void FeedTouch(int pressed, int x, int y)
{
    // 桌面可见时，先把原始喂入送给 App 宫格（它自己判定"点击"）。
    // 宫格只在位移很小且时长很短时才消费，否则让位给下面的滑动逻辑。
    if (launcher_created_) {
        app_launcher::FeedTouch(pressed, x, y);
    }

    if (pressed) {
        if (!s_pressing) {
            s_pressing = true;
            s_down_x = x;
            s_down_y = y;
        }
        s_last_x = x;
        s_last_y = y;
        return;
    }
    if (!s_pressing) {
        return;
    }
    s_pressing = false;

    const int dx = s_last_x - s_down_x;
    const int dy = s_last_y - s_down_y;
    const int threshold = std::max(kScreenW / kSwipeDiv, kSwipeMinPx);
    if (std::abs(dx) < threshold || std::abs(dx) < std::abs(dy) * 2) {
        return;   // 不以横向为主，交给 Board 的原有手势逻辑
    }

    // Coze 式桌面只有两态：表情页(0) <-> 桌面(1)。
    // App 界面(>=2)里左右滑不翻页，避免误触退出 App。
    if (s_cur_page == kEmotePage) {
        GotoPage(1);
    } else if (s_cur_page == 1) {
        GotoPage(kEmotePage);
    }
}

// ===========================================================================
// 初始化
// ===========================================================================
void InitBackend(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                 emote::EmoteDisplay* emote)
{
    s_panel_io = panel_io;
    s_panel = panel;
    s_emote = emote;
    s_gfx = static_cast<gfx_handle_t>(emote->GetEngineHandle());
    emote->SetPanelIo(panel_io);

    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_affinity = 1;   // gfx 在核 0，LVGL 放核 1
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(kScreenW * 40),
        .double_buffer = false,
        .trans_size = 0,
        .hres = kScreenW,
        .vres = kScreenH,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };
    s_lvdisp = lvgl_port_add_disp(&disp_cfg);
    if (s_lvdisp == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    // 接管 s_lvdisp 的 flush：库的 lvgl_port_flush_callback 是 static，外部拿不到，
    // 这里换成我们自己的 Real 版本（SPI 路径），以便后续在 gfx/LVGL 之间切换 flush。
    // 注意：flush_cb 必须在 lvgl_port_lock 内或在 InitBackend 的早期设置；此处
    // lvgl_port_add_disp 之后、LVGL 任务尚未开始 flush 前设置即安全。
    lv_display_set_flush_cb(s_lvdisp, EmoteAppLvglFlushReal);

    if (lvgl_port_lock(0)) {
        CreateEmotePage(emote);            // 第 0 页
        lv_screen_load(s_pages[kEmotePage].obj);
        lvgl_port_unlock();
    }

    // 表情页是默认页：把面板交给 gfx，并把 LVGL 的 flush 设为 no-op，避免
    // LVGL 后台任务与 gfx 抢同一 panel 总线（黑屏根因）。
    s_gfx_active = false;
    ActivateGfx();
    ESP_LOGI(TAG, "init done, %d page(s), default = emote", PageCount());
}

// ===========================================================================
// 内置应用页
// ===========================================================================
namespace {

void RegisterBuiltinClockPage()
{
    lv_obj_t* page = clock_page_lvgl::Create();
    if (page == nullptr) {
        ESP_LOGE(TAG, "clock page create failed");
        return;
    }
    RegisterPage(page, "clock",
                 [] { clock_page_lvgl::SetActive(true); },     // 进入：开表 + 刷一帧
                 [] { clock_page_lvgl::SetActive(false); });    // 离开：停表
    ESP_LOGI(TAG, "builtin app: clock (page %d)", PageCount() - 1);
}

}  // namespace

void InitWithBuiltinApps(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         emote::EmoteDisplay* emote)
{
    // InitBackend 内部已经把面板交给 gfx（表情页为默认页），
    // 但后续还要在 LVGL 锁内建新页面——此时 LVGL 不刷屏，只建对象是安全的。
    InitBackend(panel_io, panel, emote);
    if (s_lvdisp == nullptr) {
        return;   // InitBackend 已报错
    }
    if (lvgl_port_lock(0)) {
        RegisterBuiltinClockPage();
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "desktop ready: %d page(s) [0=emote, 1=clock]", PageCount());
}

// ===========================================================================
// Coze 式桌面
// ===========================================================================
namespace {

// 注册 Timer App 页面，返回它的页面下标。
// 必须在 InitBackend() 之后调用（需要 LVGL 已就绪）。
int RegisterClockApp()
{
    lv_obj_t* page = clock_page_lvgl::Create();
    if (page == nullptr) {
        ESP_LOGE(TAG, "clock page create failed");
        return -1;
    }
    RegisterPage(page, "clock",
                 [] { clock_page_lvgl::SetActive(true); },     // 进入：开表 + 刷一帧
                 [] { clock_page_lvgl::SetActive(false); });    // 离开：停表
    const int index = PageCount() - 1;
    ESP_LOGI(TAG, "app registered: clock (page %d)", index);
    return index;
}

}  // namespace

void InitCozeStyleDesktop(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                          emote::EmoteDisplay* emote)
{
    InitBackend(panel_io, panel, emote);
    if (s_lvdisp == nullptr) {
        return;
    }

    if (!lvgl_port_lock(0)) {
        return;
    }

    // 第 1 页：App 宫格桌面；第 2 页起：各 App 界面。
    // 顺序很重要：先建 App 页拿到它的下标，再把图标挂到桌面上，
    // 否则桌面不知道该跳到哪一页。
    const int clock_page = RegisterClockApp();

    if (clock_page >= 0) {
        app_launcher::RegisterApp({"Timer", &img_app_timer_64_64, clock_page});
    }
    // 设置页还没实现，先占位：点击仍会跳到一个空页（由 emote_app 兜底）
    // 所以暂时不注册，等实现好再加。
    (void)&img_app_settings_64_64;

    lv_obj_t* launcher = app_launcher::Create();
    if (launcher != nullptr) {
        RegisterPage(launcher, "launcher");
        launcher_created_ = true;
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "coze-style desktop ready: %d page(s) [0=emote, 1=launcher, 2=clock]",
             PageCount());
}

bool IsLauncherActive()
{
    return launcher_created_ && app_launcher::IsActive();
}

void AttachTouch(void* esp_lcd_touch_handle)
{
    // 当前实现中触摸由 Board 的触摸任务喂入 FeedTouch()，
    // 不单独注册 esp_lvgl_port 触摸驱动，避免抢 Board 的手势。
    (void)esp_lcd_touch_handle;
}

}  // namespace emote_app

#endif  // CONFIG_EMOTE_AS_LVGL_APP
