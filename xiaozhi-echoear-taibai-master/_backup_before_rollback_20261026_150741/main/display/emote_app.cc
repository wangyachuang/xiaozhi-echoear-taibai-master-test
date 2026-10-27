// ============================================================================
// emote_app.cc — gfx 表情页 / LVGL 桌面 / App 页面 之间的后端切换
// ----------------------------------------------------------------------------
// 关键事实（全部在本仓库组件源码中核实过，不是推测）
// ----------------------------------------------------------------------------
// 1) panel 完成回调是覆盖式注册
//    esp_lcd_panel_io_register_event_callbacks() 内部直接覆盖 cb + user_ctx。
//    gfx 与 esp_lvgl_port 都要 on_color_trans_done，所以同一时刻只能有一个
//    引擎往屏上刷。
//
// 2) 隐藏对象挡不住 gfx 刷屏
//    gfx_object_handler() 结尾是无条件 updated = true。
//    只要对象树非空，gfx 每轮都整屏重绘并调用 flush_cb。
//    → 停 gfx 不能靠 gfx_obj_set_visible，必须换掉 flush_cb。
//
// 3) 换 flush_cb 时不能"什么都不做"
//    gfx_refr_handler() 每块传输后等 WAIT_FLUSH_DONE，上限 50ms。
//    一帧 360x360 切成 23 块，若 flush 里既不发数据也不回完成，
//    就是 23 × 50ms ≈ 1.15s/帧，而且这段时间一直占着 SPI 和 lock_mutex。
//    → emote_display.cc 的 OnFlush 在走"覆盖"分支后会主动 flush_ready，
//      这里只需要一个空函数即可。
//
// 4) 切换必须在 gfx 的 lock_mutex 内完成
//    gfx_core_task 整个循环持 lock_mutex。持锁改 flush 覆盖 + 重注册 panel
//    回调，可保证不会在传输中途切换。
//
// 5) LVGL 侧的 flush 也要接管
//    lvgl_port_add_disp() 内部会把 on_color_trans_done 注册为 LVGL 的回调，
//    user_ctx = lv_display_t*；但它的 flush_cb（lvgl_port_flush_callback）是
//    static，外部拿不到。因此这里把 s_lvdisp 的 flush_cb 换成自己的实现：
//      - gfx 激活时：Nop（只通知 LVGL 完成、不发 SPI）
//      - LVGL 激活时：Real（走真正的 SPI 传输）
//
// 6) 切换期间必须挡住 LVGL 的刷新
//    即使 flush_cb 是 Nop，LVGL 后台任务在切换瞬间仍可能正拿着旧回调执行。
//    这里用 lvgl_port_lock()/unlock() 把"改回调 + 切页"包起来，
//    保证 LVGL 任务不会在回调被替换的过程中进入 flush。
// ============================================================================
#include "emote_app.h"

// 必须先包含 sdkconfig.h，否则下面的 CONFIG_LVGL_VERSION_MAJOR 在编译期
// 不可见，整个文件会被 #if 关掉、只剩空实现。
//（ESP-IDF 的 sdkconfig 宏不会自动进入每个翻译单元，必须显式 include。）
#include "sdkconfig.h"

// 本模块依赖 LVGL 9 + esp_lvgl_port。用工程里真实存在的宏做守卫
//（历史上用的 CONFIG_EMOTE_AS_LVGL_APP 在 Kconfig 里根本不存在，
//  导致整个文件被 #if 关掉、从未参与编译）。
#if defined(CONFIG_LVGL_VERSION_MAJOR)

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <esp_lcd_panel_io.h>
#include <esp_lvgl_port.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

#include "app_launcher.h"
#include "clock_page_lvgl.h"
#include "gfx.h"

#define TAG "EmoteApp"

// 图标描述符定义在 main/display/images/img_app_timer_64_64.c（全局命名空间）。
// 必须在全局命名空间声明，否则会被加上 emote_app:: 前缀导致链接失败。
extern "C" {
LV_IMAGE_DECLARE(img_app_timer_64_64);
}

namespace {

constexpr int kScreenW = 360;
constexpr int kScreenH = 360;

// 手势阈值
constexpr int kSwipeDiv = 6;        // 位移阈值 = 屏宽 / 6
constexpr int kSwipeMinPx = 45;     // 阈值下限
constexpr int kTapMaxMove = 24;     // 点击允许的最大位移
constexpr int64_t kTapMaxUs = 800 * 1000;

struct AppPage {
    lv_obj_t* obj = nullptr;
    const char* name = "page";
    void (*on_enter)() = nullptr;
    void (*on_leave)() = nullptr;
};

std::vector<AppPage> s_pages;
int s_cur_page = 0;
constexpr int kEmotePage = 0;

emote::EmoteDisplay* s_emote = nullptr;
gfx_handle_t s_gfx = nullptr;
lv_display_t* s_lvdisp = nullptr;
esp_lcd_panel_io_handle_t s_panel_io = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;

bool s_gfx_active = false;
bool s_ready = false;
lv_obj_t* s_blank_screen = nullptr;   // 切回表情页时用来清掉 LVGL 残留

// 触摸状态
bool s_pressing = false;
int s_down_x = 0;
int s_down_y = 0;
int s_last_x = 0;
int s_last_y = 0;
int64_t s_down_us = 0;

// ---------------------------------------------------------------------------
// panel 完成回调
// ---------------------------------------------------------------------------
// 背景（这是整块显示代码里最容易踩的坑）：
//   esp_lcd_panel_io_spi 只有【一个】on_color_trans_done 槽位 + 一个 user_ctx
//   （esp_lcd_panel_io_spi.c:167-172，重复注册只会覆盖并且打警告）。
//   而 SPI 传输是【异步排队】的：esp_lcd_panel_draw_bitmap() 把数据塞进
//   spi_device_queue_trans 就返回，真正完成是在 SPI 中断里回调
//   on_color_trans_done(&io, NULL, user_ctx)。
//
//   如果 gfx 与 LVGL 各自注册自己的回调去抢这个槽位，就会出现下面这条死锁：
//     1) LVGL 发出一个条带，tx_color 立即返回，disp->flushing 保持 1；
//     2) 条带还没传完，用户划屏，ActivateGfx() 把回调覆盖成 GfxTransDone
//        且 user_ctx 被改成 gfx 句柄；
//     3) 中断终于到来，走的是 gfx 的回调 -> lv_display_flush_ready() 永远没人调，
//        disp->flushing 卡在 1；
//     4) 下次再进桌面，lv_refr.c:1429 的 `while(disp->flushing);` 在 core1
//        上死等，taskLVGL 独占 core1 空转 -> task_wdt 反复报 IDLE1。
//
//   修法：槽位只注册一次，回调内部按"当前谁持有面板"分发；
//   并且额外记住"上一次 flush 是谁发起的"，传输跨越切换边界时也能正确投递。
// 任务与 SPI 中断共享，必须 volatile，否则编译器可能把 ISR 里的读取提升成死值。
volatile bool s_last_flush_from_lvgl = false;   // 最近一次发起传输的引擎

// 统一完成回调：按发起方归还完成信号，而不是按"当前激活方"。
bool PanelTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*)
{
    if (s_last_flush_from_lvgl) {
        lv_display_flush_ready(s_lvdisp);
    } else if (s_gfx != nullptr) {
        gfx_emote_flush_ready(s_gfx, false);
    }
    return false;
}


// gfx 挂起时的 flush：不发数据、块内部自己 flush_ready（见 emote_display.cc）。
void EmoteAppGfxFlushSuspended(void* handle, int x1, int y1, int x2, int y2, const void* data)
{
    (void)handle;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)data;
}

// 等价 lv_draw_sw_rgb565_swap（disp_cfg.flags.swap_bytes = 1）
void Rgb565SwapBytes(uint8_t* buf, uint32_t px_count)
{
    for (uint32_t i = 0; i + 1 < px_count * 2; i += 2) {
        const uint8_t t = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = t;
    }
}

void EmoteAppLvglFlushNop(lv_display_t* disp, const lv_area_t*, uint8_t*)
{
    // gfx 持有面板期间：LVGL 后台任务照常跑定时器，但不许碰 SPI。
    lv_display_flush_ready(disp);
}

void EmoteAppLvglFlushReal(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    if (px_map != nullptr && s_panel != nullptr) {
        Rgb565SwapBytes(px_map, lv_area_get_size(area));
        // 记住本次传输由 LVGL 发起：完成中断里据此归还 flush_ready，
        // 即使中途被 gfx 抢走面板也不会丢通知。
        s_last_flush_from_lvgl = true;
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    } else {
        lv_display_flush_ready(disp);
    }
}

// ---------------------------------------------------------------------------
// 启动器回调
// ---------------------------------------------------------------------------
// 注意：这些辅助函数位于匿名命名空间内，emote_app 里的符号对它们是可见的，
// 但 emote_app:: 里的函数对它们并不自动可见（未打开 namespace emote_app）。
// 所以必须写全限定名 emote_app::GotoPage，否则报
// "'GotoPage' was not declared in this scope"。
void OnLaunchApp(int page_index)
{
    emote_app::GotoPage(page_index);
}

}  // namespace

namespace emote_app {

// ===========================================================================
// 引擎切换
// ---------------------------------------------------------------------------
// 锁的纪律（两个函数必须严格遵守，否则会花屏/黑屏）：
//   * gfx_emote_lock 保护"gfx 侧"状态：SetGfxFlushOverride + panel_io 回调。
//   * lvgl_port_lock  保护"LVGL 侧"状态：lv_display 的 flush_cb。
//   * 两把锁的获取顺序固定为 gfx -> lvgl，绝不反向，避免死锁。
//   * s_gfx_active 只在 gfx_emote_lock 内改写，读它的人（IsGfxActive）
//     也在同一把锁内读，保证一致。
// ===========================================================================
void ActivateGfx()
{
    if (s_gfx == nullptr) {
        return;
    }
    gfx_emote_lock(s_gfx);   // 与 gfx_core_task 互斥，确保不在传输中途
    if (s_gfx_active) {
        gfx_emote_unlock(s_gfx);
        return;
    }

    // 先把 gfx 的对象树挂回去（引擎恢复整屏渲染），再恢复真实 flush。
    // 顺序很重要：对象挂回后引擎才会产出刷新请求，此时 flush 覆盖必须还是
    // "挂起"状态，否则第一条 flush 可能在 LVGL 还持有屏幕时抢到 SPI。
    // 所以先 ResumeEngine，再 SetGfxFlushOverride(nullptr)。
    s_emote->ResumeEngine();
    s_emote->SetGfxFlushOverride(nullptr);   // 恢复真实 flush
    // 完成回调只注册一次，永远指向统一分发器 PanelTransDone，
    // user_ctx 不再被两个引擎互相覆盖（见 PanelTransDone 上方的说明）。
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = PanelTransDone};
    esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, nullptr);

    // 面板交给 gfx 期间，LVGL 后台任务不得往同一 panel 发 SPI
    //（否则两个驱动抢总线，画面被破坏 -> 黑屏）。
    // 必须在 gfx 锁内同步完成：GotoPage(kEmotePage) 之后 lv_screen_load
    // 可能立刻触发 LVGL 刷新，晚一步就会有一帧跑到 gfx 的画面上。
    if (s_lvdisp != nullptr) {
        lvgl_port_lock(0);
        lv_display_set_flush_cb(s_lvdisp, EmoteAppLvglFlushNop);
        lvgl_port_unlock();
    }

    s_gfx_active = true;
    gfx_emote_unlock(s_gfx);
    ESP_LOGW(TAG, "backend -> GFX");
}

void ActivateLvgl()
{
    if (s_gfx == nullptr) {
        return;
    }
    gfx_emote_lock(s_gfx);
    if (!s_gfx_active) {
        gfx_emote_unlock(s_gfx);
        return;
    }

    // 1) 先让 gfx 彻底停下：把根对象从 child_list 摘下。
    //    gfx_core.c 的 gfx_object_handler() 末尾无条件 updated=true，
    //    只要对象树非空就每帧整屏重绘；child_list==NULL 是它唯一的静止条件。
    //    摘下之后 gfx 不再产生任何传输，也就不存在"完成信号该给谁"的竞态。
    s_emote->SuspendEngine();
    s_emote->SetGfxFlushOverride(EmoteAppGfxFlushSuspended);

    // 2) LVGL 侧换回真正发 SPI 的 flush —— 必须在 gfx 仍持锁时完成，
    //    这样从"gfx 停"到"LVGL 能刷"之间不存在两者都不可用的窗口期。
    if (s_lvdisp != nullptr) {
        lvgl_port_lock(0);
        lv_display_set_flush_cb(s_lvdisp, EmoteAppLvglFlushReal);
        // 关键：清掉可能残留的 flushing 标志。
        // 如果切走的那一刻还有一条 LVGL 条带在飞，而完成中断恰好落在
        // gfx 接管之后，那么 lv_display_flush_ready() 就没人调，
        // disp->flushing 会永远停在 1，下次进桌面时 lv_refr.c:1429 的
        // `while(disp->flushing);` 会把 taskLVGL 空转成看门狗风暴。
        // 这里先把标志清干净；flushing_last 由 LVGL 自己维护。
        lv_display_flush_ready(s_lvdisp);
        lvgl_port_unlock();
    }

    // 3) 完成回调无需重新注册：ActivateGfx / Init 里注册的都是统一分发器
    //    PanelTransDone，槽位保持单一，不再互相覆盖。

    s_gfx_active = false;
    gfx_emote_unlock(s_gfx);
    ESP_LOGW(TAG, "backend -> LVGL");
}

// ===========================================================================
// 页面管理
// ===========================================================================
namespace {

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
    // 注意：页面都是 lv_obj_create(nullptr) 生成的 screen，
    // 显隐只能靠 lv_screen_load() 切换。
    // 绝不能用 LV_OBJ_FLAG_HIDDEN：LVGL 9.3 的 lv_obj_remove_flag() 在处理 HIDDEN 时
    // 会调用 lv_obj_mark_layout_as_dirty(lv_obj_get_parent(obj))，
    // 而 screen 的 parent 恒为 NULL -> 解引用 NULL -> LoadProhibited 崩溃。
    s_pages.push_back(p);
}

}  // namespace

void GotoPage(int index)
{
    if (!s_ready || index < 0 || index >= static_cast<int>(s_pages.size()) || index == s_cur_page) {
        return;
    }
    const int prev = s_cur_page;

    if (index == kEmotePage) {
        // ---------------- 去表情页 ----------------
        // 此刻 panel 仍属于 LVGL：
        //   表情页本身由 gfx 画满屏，LVGL 侧不需要留任何内容，
        //   直接把 active screen 换成一个纯黑的空对象并立即刷一帧，
        //   把 LVGL 上一页的残留（文字/图标）清干净，再交还面板。
        if (s_lvdisp != nullptr) {
            lvgl_port_lock(0);
        }
        lv_obj_t* blank = s_blank_screen;
        if (blank == nullptr) {
            blank = lv_obj_create(nullptr);
            lv_obj_set_style_bg_color(blank, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
            lv_obj_remove_flag(blank, LV_OBJ_FLAG_SCROLLABLE);
            s_blank_screen = blank;
        }
        if (s_lvdisp != nullptr) {
            lv_screen_load(blank);
            lv_refr_now(s_lvdisp);
        }

        // 旧页收尾（App / 桌面的 on_leave 要在这里触发，否则定时器停不掉）
        // 这一段改对象树 + 调定时器，必须在锁内（沿用上面的 lvgl_port_lock）。
        // 旧页不需要隐藏：screen 的显隐完全由 lv_screen_load 决定。
        if (prev >= 0 && prev < static_cast<int>(s_pages.size())) {
            if (s_pages[prev].on_leave) {
                s_pages[prev].on_leave();
            }
        }

        s_cur_page = index;
        // 先放掉 LVGL 锁再交还面板：ActivateGfx 内部要拿 gfx 锁 + lvgl 锁，
        // 锁序固定为 gfx -> lvgl，这里若不先释放会形成 lvgl -> gfx 的反向序。
        if (s_lvdisp != nullptr) {
            lvgl_port_unlock();
        }
        // 面板交还 gfx
        ActivateGfx();
        ESP_LOGW(TAG, "page %d -> %d (emote)", prev, index);
        return;
    }

    // ---------------- 离开表情页 / 页面间切换 ----------------
    if (prev == kEmotePage) {
        ActivateLvgl();   // 先停 gfx
    }

    // 从这里开始就要改 LVGL 对象树（隐藏旧页 + on_leave 里的定时器/失效调用），
    // 必须持锁；否则 LVGL 后台任务正在遍历对象树时被我们改动会破坏链表。
    // 这把锁一直持有到函数末尾，覆盖下面的新页挂载与首帧刷新。
    if (s_lvdisp != nullptr) {
        lvgl_port_lock(0);
    }

    // 旧页收尾（screen 显隐交给下面的 lv_screen_load，不碰 HIDDEN 标记）
    if (prev >= 0 && prev < static_cast<int>(s_pages.size())) {
        if (prev != kEmotePage && s_pages[prev].on_leave) {
            s_pages[prev].on_leave();
        }
    }

    s_cur_page = index;

    // 新页进入
    // 此处已持有 lvgl_port_lock，直到函数末尾才释放，保证"改对象树"整段原子。
    if (s_pages[index].on_enter) {
        s_pages[index].on_enter();
    }
    lv_screen_load(s_pages[index].obj);
    lv_obj_invalidate(s_pages[index].obj);
    // 这里刻意【不】调用 lv_refr_now()：
    //   lv_refr_now 会同步阻塞地把整屏切条发给 SPI，每条 esp_lcd_panel_draw_bitmap
    //   都要等 DMA 完成，466x466 全屏约 12 个条带（buffer_size = W*40），
    //   全程阻塞在触摸任务里并持有 lvgl_port_lock。
    //   实测这会把音频通路饿死（AFE ringbuffer 持续填满：Ringbuffer of AFE(FEED) is full）。
    //   改由 LVGL 后台任务在下一轮按脏区增量刷新，触摸任务立刻返回。
    if (s_lvdisp != nullptr) {
        lv_display_trigger_activity(s_lvdisp);
    }

    // 先释放 LVGL 锁，再打日志：LOG 内部可能短暂阻塞（串口），
    // 持锁期间不应做 IO。
    if (s_lvdisp != nullptr) {
        lvgl_port_unlock();
    }
    ESP_LOGW(TAG, "page %d -> %d (%s)", prev, index, s_pages[index].name);
}

int CurrentPage()
{
    return s_cur_page;
}

bool IsEmotePageActive()
{
    return s_cur_page == kEmotePage;
}

bool IsLauncherActive()
{
    return s_cur_page == 1 && app_launcher::IsActive();
}

bool IsGfxActive()
{
    return s_gfx_active;
}

// ===========================================================================
// 触摸
// ===========================================================================
namespace {

// 在 App 页里：下滑（dy 正、纵为主）回桌面，其余忽略
bool HandleAppTouch(bool pressed, int x, int y)
{
    if (pressed) {
        if (!s_pressing) {
            s_pressing = true;
            s_down_x = x;
            s_down_y = y;
            s_down_us = esp_timer_get_time();
        }
        s_last_x = x;
        s_last_y = y;
        return true;   // App 页里按下不传给 Board（否则会触发对话）
    }
    if (!s_pressing) {
        return true;
    }
    s_pressing = false;

    // 抬起点一律以本次传入的 x,y 为准（调用方只保证给按下/抬起两次事件，
    // s_last_* 在按下时等于起点，直接读会让 dx/dy 恒为 0）。
    s_last_x = x;
    s_last_y = y;

    const int dx = s_last_x - s_down_x;
    const int dy = s_last_y - s_down_y;
    const int64_t held_us = esp_timer_get_time() - s_down_us;
    const int swipe_th = std::max(kScreenW / kSwipeDiv, kSwipeMinPx);

    // 下滑回桌面
    if (dy > swipe_th && std::abs(dy) > std::abs(dx) * 2) {
        GotoPage(1);
        return true;
    }

    // 短按：Timer 页切数字/模拟面
    if (std::abs(dx) <= kTapMaxMove && std::abs(dy) <= kTapMaxMove && held_us <= kTapMaxUs) {
        clock_page_lvgl::ToggleFace();
        return true;
    }
    return true;
}

}  // namespace

bool FeedTouch(bool pressed, int x, int y)
{
    ESP_LOGW(TAG, "FeedTouch(pressed=%d x=%d y=%d) ready=%d page=%d", pressed, x, y,
             static_cast<int>(s_ready), s_cur_page);
    if (!s_ready) {
        return false;
    }

    // ---------- App 页（>= 2） ----------
    if (s_cur_page >= 2) {
        return HandleAppTouch(pressed, x, y);
    }

    // ---------- 桌面页 ----------
    if (s_cur_page == 1) {
        // 宫格点击优先；它只在"短按 + 落在图标上"时才消费。
        // 注意：命中图标时 s_launch 内部会 GotoPage(2) 把页面切走，
        // 所以必须在同一个分支里就 return，不能再落到下面的通用滑动检测
        //（否则会用旧坐标再跳一次页）。
        if (app_launcher::FeedTouch(pressed, x, y)) {
            s_pressing = false;   // 丢弃这次按下，避免残留状态干扰后续手势
            return true;
        }
        if (s_cur_page != 1) {
            // 防御：若将来 app_launcher 改成"按下即预切换"，这里同样要退出。
            s_pressing = false;
            return true;
        }
    }

    // ---------- 通用手势检测 ----------
    // 框架约定（务必遵守，否则滑动恒为 0）：
    //   调用方每次触摸只保证给两次事件：
    //     FeedTouch(true , 按下点x, 按下点y)
    //     FeedTouch(false, 抬起点x, 抬起点y)
    //   中间不保证有任何移动事件。所以
    //     * 按下：只记录起点 s_down_*；
    //     * 抬起：必须用【本次传入的 x,y】当终点算位移，
    //             绝不能读 s_last_*（它只在按下时被赋值，等于起点，
    //             算出来的 dx/dy 永远是 0 —— 这正是之前滑不动的根因）。
    if (pressed) {
        if (!s_pressing) {
            s_pressing = true;
            s_down_x = x;
            s_down_y = y;
            s_down_us = esp_timer_get_time();
        }
        s_last_x = x;
        s_last_y = y;
        return false;
    }

    if (!s_pressing) {
        return false;
    }
    s_pressing = false;

    // 抬起点一律以本次参数为准
    s_last_x = x;
    s_last_y = y;

    const int dx = s_last_x - s_down_x;
    const int dy = s_last_y - s_down_y;
    const int swipe_th = std::max(kScreenW / kSwipeDiv, kSwipeMinPx);

    // 不以横向为主 → 交回 Board 做点击/唤醒
    if (std::abs(dx) < swipe_th || std::abs(dx) < std::abs(dy) * 2) {
        ESP_LOGW(TAG, "FeedTouch reject: dx=%d dy=%d th=%d down=(%d,%d) last=(%d,%d) page=%d",
                 dx, dy, swipe_th, s_down_x, s_down_y, s_last_x, s_last_y, s_cur_page);
        return false;
    }
    ESP_LOGW(TAG, "FeedTouch accept swipe dx=%d dy=%d page=%d", dx, dy, s_cur_page);

    // 表情页 <-> 桌面
    if (s_cur_page == kEmotePage) {
        GotoPage(1);
    } else if (s_cur_page == 1) {
        GotoPage(kEmotePage);
    }
    return true;
}

// ===========================================================================
// 初始化
// ===========================================================================
namespace {

lv_obj_t* CreateEmotePlaceholderPage()
{
    // 表情页在 LVGL 里只是一张黑底占位：真正的画面由 gfx 画。
    // 这样切换时的 lv_screen_load 有对象可指，也方便未来加角标。
    lv_obj_t* page = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

void CreateTimerPage()
{
    lv_obj_t* page = clock_page_lvgl::Create();
    if (page == nullptr) {
        ESP_LOGE(TAG, "Timer page create failed");
        return;
    }
    RegisterPage(page, "Timer", [] { clock_page_lvgl::SetActive(true); },
                 [] { clock_page_lvgl::SetActive(false); });
    ESP_LOGW(TAG, "app registered: Timer (page %d)", static_cast<int>(s_pages.size()) - 1);
}

}  // namespace

void InitCozeStyleDesktop(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                          emote::EmoteDisplay* emote)
{
    // 入口必打（WARN 级）：本工程 CONFIG_LOG_DEFAULT_LEVEL_WARN=y，
    // INFO 级日志默认被丢弃，所以关键路径一律用 ESP_LOGW。
    ESP_LOGW(TAG, "InitCozeStyleDesktop enter (io=%p panel=%p emote=%p)", panel_io, panel,
             static_cast<void*>(emote));

    if (s_ready) {
        ESP_LOGW(TAG, "already initialized");
        return;
    }
    if (emote == nullptr) {
        ESP_LOGE(TAG, "emote display is null");
        return;
    }

    s_panel_io = panel_io;
    s_panel = panel;
    s_emote = emote;
    s_gfx = static_cast<gfx_handle_t>(emote->GetEngineHandle());
    emote->SetPanelIo(panel_io);

    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_affinity = 1;   // gfx 在核 0，LVGL 放核 1
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        ForceGfxOnly();
        return;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(kScreenW * 40),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(kScreenW),
        .vres = static_cast<uint32_t>(kScreenH),
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
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
        ForceGfxOnly();
        return;
    }

    // 接管 LVGL 的 flush，使切换完全可控
    lv_display_set_flush_cb(s_lvdisp, EmoteAppLvglFlushReal);

    // lvgl_port_add_disp() 内部会调用 lvgl_port_add_disp 的注册逻辑，
    // 把 panel_io 的 on_color_trans_done 抢成它自己的 lvgl_flush_io_ready_callback。
    // 这里立刻换回统一分发器 PanelTransDone，否则 gfx 的完成通知会被它吃掉，
    // gfx_core 每块都要空等 50ms 超时。
    {
        const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = PanelTransDone};
        esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, nullptr);
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        ForceGfxOnly();
        return;
    }

    // 第 0 页：表情占位
    RegisterPage(CreateEmotePlaceholderPage(), "emote", nullptr, nullptr);

    // 第 1 页：桌面
    app_launcher::SetLaunchCallback(OnLaunchApp);
    lv_obj_t* launcher = app_launcher::Create();
    RegisterPage(launcher, "launcher", [] { app_launcher::OnEnter(); },
                 [] { app_launcher::OnLeave(); });

    // 第 2 页起：App
    CreateTimerPage();

    // 挂 Timer 图标
    app_launcher::RegisterApp({"Timer", &img_app_timer_64_64, 2});

    lvgl_port_unlock();

    s_ready = true;
    s_cur_page = kEmotePage;

    // 默认停在表情页：把面板交给 gfx（ActivateGfx 内部已同步把 LVGL
    // 的 flush 切成 no-op，不需要再补一次）。
    ActivateGfx();

    ESP_LOGW(TAG, "coze-style desktop ready: %d page(s), default = emote",
             static_cast<int>(s_pages.size()));
}

bool IsReady()
{
    return s_ready;
}

void ForceGfxOnly()
{
    // 把面板无条件还给 gfx；LVGL 若已起来则进入 no-op，不再碰 SPI。
    if (s_lvdisp != nullptr) {
        lvgl_port_lock(0);
        lv_display_set_flush_cb(s_lvdisp, EmoteAppLvglFlushNop);
        lvgl_port_unlock();
    }
    if (s_emote != nullptr) {
        // 兜底路径也必须把对象树挂回去，否则一旦走过这里，
        // gfx 会永久停在"已挂起"状态，表情再也不显示。
        s_emote->ResumeEngine();
        s_emote->SetGfxFlushOverride(nullptr);
    }
    if (s_gfx != nullptr && s_panel_io != nullptr) {
        gfx_emote_lock(s_gfx);
        const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = PanelTransDone};
        esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, nullptr);
        gfx_emote_unlock(s_gfx);
    }
    s_gfx_active = true;
    s_ready = false;
    ESP_LOGW(TAG, "forced back to gfx-only mode");
}

}  // namespace emote_app

#endif  // CONFIG_LVGL_VERSION_MAJOR
