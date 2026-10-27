// ============================================================================
// page_manager.cc — 页面管理器实现
// ============================================================================
#include "page_manager.h"

#include "emote_display.h"
#include "launcher_page.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>

#include <cstring>
#include <atomic>

// 全局默认日志级别是 WARN（见 sdkconfig: CONFIG_LOG_DEFAULT_LEVEL_WARN），
// ESP_LOGI 不会输出。页面切换属于关键流程，这里统一用 WARN 级别打印，
// 保证串口里能看到每一次切换，便于定位"卡在哪一步"。
#define TAG "PageManager"

namespace coze_ui {

namespace {

// ★ 版本指纹：每次改完代码就把这里的日期改一下，烧录后串口会打印
//   "==== coze_ui build: YYYY-MM-DD ... ===="
// 这样能立刻分辨"新固件真的跑起来了"还是"烧的还是旧固件 / 编译没生效"。
// 曾经因为分不清这两种情况，误判过好几轮。
constexpr const char* kBuildStamp = "2026-10-27 tap-edge-queue";

// LVGL 缓冲区：360 x 40 行 RGB565 = 28800 B。与 lvgl_clock_probe 验证过的
// 参数一致（已验证可在该 panel 上正常刷屏）。
constexpr int kLvBufferRows = 40;

// 切换时应等待的 LVGL 锁超时。切后端是低频操作（人手滑动），给足余量，
// 但必须有上限：LVGL 任务若因其它原因卡死，触摸任务不能一起陪葬。
constexpr uint32_t kLvglLockTimeoutMs = 1000;

// 页面管理自己的互斥量超时
constexpr uint32_t kMutexTimeoutMs = 2000;

// 触摸点状态：由板级触摸任务写入，由 LVGL 的 indev_read_cb 在 LVGL 任务里读出。
// 两者跨任务，所以用原子量而不是普通变量。
std::atomic<int32_t> g_touch_x{0};
std::atomic<int32_t> g_touch_y{0};

// 按下边沿锁存。
//
// 背景（已更正）：本工程实际用的是 esp_lvgl_port 的 LVGL9 分支（lvgl 9.3.0），
//      lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, ...) 是完整实现，能真正置
//      TOUCH 事件位、唤醒 LVGL 任务去调 lv_indev_read()。之前的"LVGL8 空实现"
//      结论是误读了 src/lvgl8/ 分支导致的错误诊断，已撤销。
//
//      真正的根因是：LVGL9 下 lv_indev_read() 【不会】被 lv_timer_handler 的
//      indev read_timer 驱动 —— lvgl_port_task 只在收到 LVGL_PORT_EVENT_TOUCH
//      事件位时才显式遍历 indev 并读取（src/lvgl9/esp_lvgl_port.c:224）。而
//      FeedTouch() 里那次唤醒调用曾被基于错误结论删掉，导致触摸点喂进去了
//      却从不被 LVGL 读取，PRESSED/CLICKED 一个都不触发。
//
// ★ 2026-10-27 最终修法（"单次点击进不去、连点几次才进得去"）：
//
// 走过的两次弯路，记下来避免再犯：
//   弯路 1：删掉 FeedTouch 里的 lvgl_port_task_wake()（误信"LVGL8 空实现"），
//           导致 LVGL 从不读 indev，任何点击都没反应。
//   弯路 2：加了 g_tap_pending 边沿锁存 + g_release_burst 强制松开，试图
//           "补偿"怀疑丢失的触摸边沿。结果补偿逻辑本身才是问题：
//           每次新按下先插一轮 RELEASED，把消化该次按下的那一轮吃掉了；
//           而手指松开后 FeedTouch 不再被调用、LVGL 也就不会被唤醒去做
//           第二轮 PRESSED —— 这笔账永远还不清。表现就是"单点无效、
//           连点若干次偶尔凑巧成功"。
//
// 现在的模型：indev 只如实汇报【当前电平】，但保证每次按下/松开都被 LVGL 读到。
//
//   为什么光靠事件位不够：lvgl_port_task 用 xEventGroupWaitBits(..., pdTRUE, ...)
//   读事件位并【清除全部位】（src/lvgl9/esp_lvgl_port.c:219）。如果按下和松开
//   发生在 LVGL 任务两次唤醒之间的同一个间隙里（日志里常见同毫秒 PRESS+RELEASE），
//   两次唤醒会被合并成一次，LVGL 只读到最终的"已松开"，于是只派发 RELEASED，
//   没有 PRESSED —— CLICKED 自然不成立。这就是"点一次没反应"的最后一块拼图。
//
//   解法：一个深度 2 的边沿队列。FeedTouch 每次状态跳变（按下/松开）就压入一个
//   快照；IndevReadCb 先消费队列里的快照（保证按下与松开各被读一次），
//   队列空时才汇报当前电平（长按期间的持续 PRESSED 由此得到）。
constexpr int kEdgeQueueDepth = 2;
struct TouchEdge {
    bool     valid = false;
    bool     pressed = false;
    int32_t  x = 0;
    int32_t  y = 0;
};
std::atomic<TouchEdge> g_edge_q[kEdgeQueueDepth];
std::atomic<int>       g_edge_head{0};   // 下一个要读的下标
std::atomic<int>       g_edge_tail{0};   // 下一个要写的下标

std::atomic<bool> g_touch_valid{false};
std::atomic<bool> g_touch_pressed{false};

// ----------------------------------------------------------------------------
// 触摸链路诊断（定位"点一下进不去"到底断在哪一环）
// ----------------------------------------------------------------------------
// 整条链路：触摸芯片 -> EchoEar touch_task -> FeedTouch() -> 原子变量
//           -> lvgl_port_task_wake() -> LVGL 任务调 lv_indev_read()
//           -> IndevReadCb() -> lv_indev 派发 PRESSED/RELEASED/CLICKED
//
// FEED 行 = 触摸任务真的把点喂进来了；
// READ  行 = LVGL 任务真的读了 indev。
// 有 FEED 无 READ  -> 断在唤醒/调度（LVGL 任务没跑 lv_indev_read）；
// 有 READ 无图标事件 -> 断在 LVGL 派发（坐标打空 / 对象不可点）。
std::atomic<uint32_t> g_indev_read_count{0};
std::atomic<uint32_t> g_feed_count{0};
// 上次 READ 的状态，用于"只在变化时打印"，避免日志淹没串口
std::atomic<int> g_last_read_state{-1};

// 只在 state 变化时打印，另加每次都被调用的计数。
void LogRead(const uint32_t n, const bool pressed, const int32_t x, const int32_t y,
             const char* via)
{
    const int now = pressed ? 1 : 0;
    const int prev = g_last_read_state.exchange(now, std::memory_order_relaxed);
    if (prev != now) {
        ESP_LOGW(TAG, "READ #%u state=%s (%d,%d) [%s]",
                 static_cast<unsigned>(n), pressed ? "PRESSED" : "RELEASED",
                 static_cast<int>(x), static_cast<int>(y), via);
    }
}

// LVGL 指针设备回调：优先消费边沿队列，其次汇报当前电平。
// lv_indev_data_t 的 state 是【电平】语义：按住期间必须一直是 PRESSED，
// 松开期间是 RELEASED。
void IndevReadCb(lv_indev_t* indev, lv_indev_data_t* data)
{
    (void)indev;

    // 先看有没有还没被读到的状态跳变
    const int head = g_edge_head.load(std::memory_order_acquire);
    const int tail = g_edge_tail.load(std::memory_order_acquire);
    if (head != tail) {
        const TouchEdge e = g_edge_q[head].load(std::memory_order_acquire);
        if (e.valid) {
            g_edge_head.store((head + 1) % kEdgeQueueDepth, std::memory_order_release);
            data->point.x = e.x;
            data->point.y = e.y;
            data->state = e.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            const uint32_t n = g_indev_read_count.fetch_add(1) + 1;
            LogRead(n, e.pressed, e.x, e.y, "edge");
            return;
        }
        // 槽位还没写好（理论上不会发生），保守地按当前电平走
    }

    data->point.x = static_cast<int32_t>(g_touch_x.load(std::memory_order_relaxed));
    data->point.y = static_cast<int32_t>(g_touch_y.load(std::memory_order_relaxed));
    const bool lvl = g_touch_pressed.load(std::memory_order_relaxed);
    data->state = lvl ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    const uint32_t n = g_indev_read_count.fetch_add(1) + 1;
    LogRead(n, lvl, data->point.x, data->point.y, "level");
}

PageManager* g_instance = nullptr;

// LVGL 侧 flush 通道要用到的硬件句柄（文件作用域，避免自由函数访问私有成员）。
esp_lcd_panel_handle_t g_panel = nullptr;
lv_display_t* g_disp = nullptr;

// ----------------------------------------------------------------------------
// LVGL 的 flush 通道（自建 lv_display，不经过 esp_lvgl_port）
// ----------------------------------------------------------------------------
// 为什么不直接用 lvgl_port_add_disp：它内部无条件调用
// esp_lcd_panel_io_register_event_callbacks()，把 gfx 唯一的完成回调顶掉
// （串口里会出现 "Callback on_color_trans_done was already set and now it was
// overwritten!"）。顶掉之后 gfx 的 flush_ready 永远等不到，表情就一帧一帧卡死。
//
// 现在改成：panel_io 的回调槽位永远留给 gfx，LVGL 这边自己建 display，
// 发起传输前用 GfxSetFlushOwnerLvgl() 声明归属，传输完成时由 gfx 的 ISR
// 转发到 LvglFlushDoneCb()。

// LVGL 侧 flush 通道的统计，用于定位"刷不出来/卡"这类问题。
// 计数只在 flush 回调里递增，串口里由 DumpStatsOnExit 在离开桌面时打印一次。
std::atomic<uint32_t> g_flush_blocks{0};
std::atomic<uint32_t> g_flush_errors{0};
// 本次进桌面期间 LVGL 实际画到面板上的像素总数。
// 全屏一帧 = 360*360 = 129600；若这个数明显小于 129600 的整数倍，
// 说明只刷了一部分区域，屏幕会被"撕成几条"。
std::atomic<uint32_t> g_flush_px{0};
// flush 等待超时次数：正常应为 0；若持续增长说明完成中断经常丢，
// 需要回头查 panel_io 的 ISR 路由（g_flush_owner 仲裁）。
std::atomic<uint32_t> g_flush_timeouts{0};
// flush 完成信号量与等待超时（详见下方 LvglFlushWaitCb 的说明）。
// 声明必须早于 LvglFlushDoneCb：ISR 里要 give 它。
SemaphoreHandle_t g_flush_done_sem = nullptr;
constexpr uint32_t kFlushWaitTimeoutMs = 100;
// 切回 gfx 前等待"LVGL 在途 DMA 全部落地"的上限。
// 正常情况下 9 块 × 40 行 ~= 0.8ms/块，几毫秒内就能排空；
// 上限只是兜底，超时不会崩，只会退化成"可能有一帧错位"。
constexpr uint32_t kDrainTimeoutMs = 100;
// 只打印第一次 flush 的区域，用于确认 LVGL 确实在画。
bool g_first_flush = true;

// 对应 gfx 侧的 flags.swap = true：面板期望 RGB565 高字节在前，
// 而 LVGL 画出来的是本机字节序，必须交换，否则红蓝颠倒。
void LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    if (g_panel == nullptr) {
        // 没有面板：也必须结束本次 flush，否则 LVGL 会永远等下去。
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    if (w <= 0 || h <= 0) {
        lv_display_flush_ready(disp);
        return;
    }

    lv_draw_sw_rgb565_swap(px_map, static_cast<uint32_t>(w) * static_cast<uint32_t>(h));

    // 统计：这一笔确实是发往面板的，不是被 LVGL 判定为"不可见"跳过的
    g_flush_blocks.fetch_add(1, std::memory_order_relaxed);

    // 声明"这笔传输归 LVGL"，必须写在 draw_bitmap 之前：
    // DMA 可能在 draw_bitmap 返回后立刻完成并进中断，
    // 那时若标记还没更新，ISR 会把完成事件错发给 gfx。
    emote::GfxSetFlushOwnerLvgl();
    // 记账：与 gfx 侧共用同一个在途计数。销毁引擎前的"等总线空闲"
    // 依赖这个计数准确 —— 漏记会让计数变负，drain 立即返回，
    // 残留的 flush_ready 命令就会在引擎销毁后去访问已删除的 event group。
    emote::GfxSpiInflightAdd();

    // 清掉可能残留的完成信号。
    // 场景：上一块等了 kFlushWaitTimeoutMs 超时放行，之后 ISR 才迟到 give，
    // 这个 token 会留在二值信号量里。不清掉的话下面这次 take 会立刻通过，
    // 本块的 DMA 就没人等了 —— 撕裂会以"偶发"的形式重现，最难查。
    if (g_flush_done_sem != nullptr) {
        while (xSemaphoreTake(g_flush_done_sem, 0) == pdTRUE) {
            // 一直取到空为止
        }
    }

    g_flush_px.fetch_add(static_cast<uint32_t>(w) * static_cast<uint32_t>(h),
                         std::memory_order_relaxed);
    if (g_first_flush) {
        g_first_flush = false;
        ESP_LOGW(TAG, "first lvgl flush: area=(%d,%d)-(%d,%d) w=%d h=%d",
                 (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2, (int)w, (int)h);
    }

    // 发起传输，然后【在本函数内阻塞等到 DMA 完成】再返回。
    //
    // 为什么必须同步等（这里是"点 Timer 出现撕裂时钟"的根因）：
    //   LVGL 是单缓冲（buffer_size = 360x40，double_buffer=false）。
    //   flush_cb 一旦返回，LVGL 就认为这块绘制缓冲可以复用了 —— 下一块
    //   甚至下一帧会直接往同一片内存里画。而 DMA 是异步的，很可能还在传。
    //   结果就是屏幕上同时出现"上一帧的上半部"和"新一帧的下半部"，
    //   表现为文字被撕成两半、时钟上下错位。
    //
    //   原先的做法是 flush_cb 立即返回、把 flush_ready 丢给 ISR 里的
    //   LvglFlushDoneCb。那个做法只在双缓冲下安全，单缓冲必然撕裂。
    //
    //   同步等的代价：LVGL 的渲染任务会在这里被阻塞约 0.1~0.8ms/块。
    //   桌面是静态画面，这点开销完全感知不到；而撕裂是肉眼可见的。
    const esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, area->x1, area->y1,
                                                    area->x2 + 1, area->y2 + 1, px_map);
    if (err != ESP_OK) {
        // 传输根本没发出去：完成中断永远不会来，必须在这里补一次 ready，
        // 否则 LVGL 会一直等，整个界面冻住。
        // 同时把刚才的记账回滚，否则计数永久 +1，销毁引擎前的 drain 会卡到超时。
        g_flush_errors.fetch_add(1, std::memory_order_relaxed);
        emote::GfxSpiInflightDone();
        emote::GfxSetFlushOwnerGfx();
        lv_display_flush_ready(disp);
        return;
    }

    // 等这一块真正传完。完成后 ISR 会 give g_flush_done_sem 并顺手调
    // lv_display_flush_ready（见 LvglFlushDoneCb），所以这里只需等信号量。
    // 超时不是错误：说明这一块的完成中断没等到，直接放行继续，
    // 由 LVGL 决定下一块怎么画。绝不能用 portMAX_DELAY —— 那会把
    // "丢一次中断"放大成"永久卡死"。
    if (g_flush_done_sem != nullptr) {
        if (xSemaphoreTake(g_flush_done_sem, pdMS_TO_TICKS(kFlushWaitTimeoutMs)) != pdTRUE) {
            g_flush_timeouts.fetch_add(1, std::memory_order_relaxed);
            // 没等到中断也必须把 LVGL 的 flushing 标志清掉，否则本帧永远结束不了。
            lv_display_flush_ready(disp);
        }
    } else {
        lv_display_flush_ready(disp);
    }
}

// 在 panel_io 的完成 ISR 里被调用（由 EmoteEngine::OnFlushIoReady 转发）。
// 只做一件事：告诉 LVGL 这一块传完了，可以继续下一块/结束本帧。
//
// 注意这里【同时】做两件事：
//   1) give 信号量 —— 让阻塞在 flush_wait_cb 里的 LVGL 任务立刻醒来；
//   2) 仍然调 lv_display_flush_ready —— 兼容 LVGL 内部的 flushing 标志，
//      否则 wait_for_flushing 里的 flush_wait_cb 分支虽然会清 flushing，
//      但 flush_ready 这一步不做会让 LVGL 认为缓冲仍在使用中。
// 两者在 ISR 里都是安全的：xSemaphoreGiveFromISR 是中断安全的，
// lv_display_flush_ready 本身也带 LV_ATTRIBUTE_FLUSH_READY（LVGL 明确它可在 ISR 调用）。
void LvglFlushDoneCb()
{
    if (g_flush_done_sem != nullptr) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(g_flush_done_sem, &woken);
        if (woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
    if (g_disp != nullptr) {
        lv_display_flush_ready(g_disp);
    }
}

// ----------------------------------------------------------------------------
// flush 等待：二值信号量（替代 LVGL 默认的 while(disp->flushing) 空转）
// ----------------------------------------------------------------------------
// LVGL 的 wait_for_flushing() 在没有 flush_wait_cb 时是一句裸的
//   while(disp->flushing);
// 而 flushing 是 lv_display_t 里的普通 int，没有任何内存序/原子性保证，
// 只在 flush_ready 里被写成 0。串口里那串
//   task_wdt: CPU 1: taskLVGL  +  wait_for_flushing at lv_refr.c:1429
// 就是这句空转把 CPU1 吃满 5 秒以上。
//
// 更关键的是它会把"ISR 里调 lv_display_flush_ready"这件事变成竞态：
// DMA 完成中断可能比 draw buf flush 的时序慢一拍，一旦某次完成事件丢失，
// flushing 就永远是 1，LVGL 任务彻底卡死，且无法自愈。
//
// 因此这里注册 flush_wait_cb：完成中断只 give 信号量，
// LVGL 任务在信号量上阻塞等待，带超时兜底。即使丢一次中断，
// 也只是这一帧多等一个超时周期，不会永久卡死。
void LvglFlushWaitCb(lv_display_t* disp)
{
    (void)disp;
    // 同步刷新模式下，这个回调基本不会被等到了：
    // LvglFlushCb 已经在 flush_cb 内部阻塞到 DMA 完成才返回，
    // LVGL 走到 wait_for_flushing 时 disp->flushing 已经是 0。
    //
    // 这里刻意【不再】去 take 信号量。原因：
    //   flush_cb 里已经 take 过一次，这里再 take 一次就会出现
    //   "一块数据等两次信号量"的错位 —— 第二次 take 会把下一块
    //   或下一帧的信号量提前吃掉，导致真正该等的 DMA 没人等，
    //   撕裂又会回来。这个函数保留只是为了不让 flush_wait_cb
    //   落到 LVGL 默认的 while(disp->flushing) 空转实现上。
    //
    // 同步模式下 flush_cb 已经阻塞到 DMA 完成才返回，LVGL 走到这里时
    // 那一块早传完了，所以这里【什么也不用做】—— 不是遗忘，
    // 而是刻意不碰信号量、也不碰 disp->flushing（后者是 lv_display_t
    // 的私有字段，对上层是不完整类型，根本访问不到）。
    // 保留本函数的唯一目的：让 lv_display_set_flush_wait_cb 有一个有效实现，
    // 从而避免 LVGL 回落到 while(disp->flushing) 的空转实现。
    (void)disp;
}

}  // namespace

PageManager& PageManager::GetInstance()
{
    // C++11 起的函数内静态初始化是线程安全的（首次调用时由编译器插入 guard），
    // 这里不依赖构造函数做任何硬件操作，只建一个空对象，安全。
    static PageManager instance;
    g_instance = &instance;
    return instance;
}

PageManager::~PageManager()
{
    for (UiPage* p : pages_) {
        delete p;
    }
    pages_.clear();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

esp_err_t PageManager::Init(emote::EmoteDisplay* display,
                            esp_lcd_panel_io_handle_t panel_io,
                            esp_lcd_panel_handle_t panel,
                            int width, int height)
{
    if (inited_) {
        ESP_LOGW(TAG, "Init called twice, ignored");
        return ESP_OK;
    }
    if (display == nullptr || panel_io == nullptr || panel == nullptr) {
        ESP_LOGE(TAG, "Init: null handle (display=%p io=%p panel=%p)",
                 (void*)display, (void*)panel_io, (void*)panel);
        return ESP_ERR_INVALID_ARG;
    }

    display_ = display;
    panel_io_ = panel_io;
    panel_ = panel;
    width_ = width;
    height_ = height;

    // 版本指纹：每次改动后改一下这行，烧录后看串口就知道新固件有没有真的跑起来。
    // （曾经因为"改了代码但看到的界面没变化"浪费过一轮排查时间。）
    ESP_LOGW(TAG, "==== coze_ui build: %s ====", kBuildStamp);
    ESP_LOGW(TAG, "panel %dx%d, gfx=%p emote=%p", width, height, (void*)panel, (void*)display);

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        ESP_LOGE(TAG, "Init: create mutex failed");
        return ESP_ERR_NO_MEM;
    }

    // ---- LVGL 库 + port 初始化（此时还不挂 display，不占 DMA 缓冲）----
    // 注意：EchoEar 走 EmoteDisplay 分支，此前从未调用过 lv_init()。
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;   // 与 lcd_display.cc 保持一致
    port_cfg.task_affinity = 1;   // 放核心 1，避开 gfx 任务（核心 0）
    const esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        // 失败也要继续：只是没有桌面可用，表情页必须照常工作。
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        lvgl_ready_ = false;
    } else {
        lvgl_ready_ = true;
        ESP_LOGW(TAG, "lvgl_port_init OK (display deferred until first swipe)");
    }

    inited_ = true;

    // 注册内置页面：launcher 必须在最前，它是"桌面"这个概念本身的载体
    launcher_ = new LauncherPage();
    RegisterPage(launcher_);
    current_ = launcher_;

    ESP_LOGW(TAG, "Init done: %dx%d, pages=%u", width_, height_,
             (unsigned)pages_.size());
    return ESP_OK;
}

void PageManager::RegisterPage(UiPage* page)
{
    if (page == nullptr) {
        return;
    }
    pages_.push_back(page);

    // launcher 以"图标表"的形式承载所有非 launcher 页面。
    // 这里只登记，真正的图标在 LauncherPage::Build() 里生成。
    //
    // 注意：工程默认编译选项是 -fno-rtti，dynamic_cast 不能用，
    // 这里直接用指针比较即可（launcher_ 本身就是 LauncherPage* 的基类指针）。
    if (launcher_ != nullptr && page != launcher_) {
        auto* launcher = static_cast<LauncherPage*>(launcher_);
        LauncherPage::Entry e;
        e.icon = nullptr;          // 图标资源由 LauncherPage::Build 按名字查找
        e.name = page->Name();
        e.target = page;
        launcher->AddApp(e);
    }
}

void PageManager::SetGfxMuteHook(GfxMuteFn fn, void* ctx)
{
    gfx_mute_ = fn;
    gfx_mute_ctx_ = ctx;
}

void PageManager::SetBatteryHook(BatteryFn fn, void* ctx)
{
    battery_ = fn;
    battery_ctx_ = ctx;
}

int PageManager::BatteryPercent() const
{
    if (battery_ == nullptr) {
        return -1;
    }
    return battery_(battery_ctx_);
}

// ----------------------------------------------------------------------------
// LVGL display 惰性创建
// ----------------------------------------------------------------------------
bool PageManager::EnsureLvglDisplay()
{
    if (lv_disp_ != nullptr) {
        return true;
    }
    if (!lvgl_ready_) {
        ESP_LOGE(TAG, "EnsureLvglDisplay: lvgl port not ready");
        return false;
    }

    // ---- 缓冲区 ----
    // 必须是 DMA 可访问的内存，否则 esp_lcd_panel_draw_bitmap 会直接返回
    // ESP_ERR_INVALID_ARG，表现为"LVGL 页面全黑"。
    // 与 lvgl_port 一致：MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL，按 cache line 对齐。
    const uint32_t stride = static_cast<uint32_t>(width_) * 2u;  // RGB565 每像素 2B
    const uint32_t buf_size = stride * static_cast<uint32_t>(kLvBufferRows);
    lv_color_t* lv_buf = static_cast<lv_color_t*>(
        heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (lv_buf == nullptr) {
        ESP_LOGE(TAG, "lv buffer alloc failed (%u B)", static_cast<unsigned>(buf_size));
        return false;
    }

    lv_disp_ = lv_display_create(width_, height_);
    if (lv_disp_ == nullptr) {
        ESP_LOGE(TAG, "lv_display_create failed");
        heap_caps_free(lv_buf);
        return false;
    }
    lv_display_set_color_format(lv_disp_, LV_COLOR_FORMAT_RGB565);
    // 单缓冲、PARTIAL 模式：LVGL 画满 kLvBufferRows 行就调用一次 flush。
    lv_display_set_buffers(lv_disp_, lv_buf, nullptr, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lv_disp_, LvglFlushCb);
    lv_display_set_user_data(lv_disp_, this);

    // 关键：禁用 LVGL 默认的 while(flushing) 空转，改用二值信号量阻塞等待。
    // 没有这一步时，任何一次 DMA 完成中断丢失都会让 LVGL 任务
    // 在 lv_refr.c:1429 永久空转，直接触发 task_wdt（历史事故）。
    if (g_flush_done_sem == nullptr) {
        g_flush_done_sem = xSemaphoreCreateBinary();
        if (g_flush_done_sem == nullptr) {
            ESP_LOGE(TAG, "flush done semaphore create failed");
            return false;
        }
    }
    lv_display_set_flush_wait_cb(lv_disp_, LvglFlushWaitCb);

    // flush 回调与完成回调都走文件作用域句柄，这里同步给它们
    g_panel = panel_;
    g_disp = lv_disp_;

    // LVGL 的 flush 完成由 panel_io 的完成 ISR 经由 gfx 的槽位回调进来。
    // 这里只登记"完成时通知谁"，绝不重注册 panel_io 回调（那会顶掉 gfx）。
    emote::GfxSetLvglFlushDoneHandler(&LvglFlushDoneCb);

    // 立刻停掉 LVGL 的 tick，保证回到表情页时它不会偷刷。
    lvgl_port_stop();
    lvgl_ticking_ = false;

    lv_root_ = lv_display_get_screen_active(lv_disp_);
    lv_obj_set_style_bg_color(lv_root_, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_root_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(lv_root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 指针设备 ----
    // 本工程没有让 esp_lvgl_port 自带触摸驱动接管：CST816S 由 EchoEar 的
    // touch_task 独占读取（里面还有手势判定），再加一个 I2C 消费者会打架。
    // 因此注册一个"外部喂点"的 indev，坐标由 FeedTouch() 推进来。
    if (lv_indev_ == nullptr) {
        lv_indev_ = lv_indev_create();
        if (lv_indev_ != nullptr) {
            lv_indev_set_type(lv_indev_, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(lv_indev_, IndevReadCb);
            lv_indev_set_display(lv_indev_, lv_disp_);
        } else {
            ESP_LOGE(TAG, "lv_indev_create failed: touch will not reach widgets");
        }
    }

    ESP_LOGW(TAG, "lvgl display created (buf=%d rows)", kLvBufferRows);
    return true;
}

void PageManager::FeedTouch(int x, int y, bool pressed)
{
    // 仅在桌面（LVGL）模式下喂点。返回表情页后残留的按下状态会让
    // 下次进桌面时 LVGL 以为手指还按着，直接把第一个图标"按"下去。
    if (!in_lvgl_ || lv_indev_ == nullptr) {
        return;
    }
    g_touch_x.store(x, std::memory_order_relaxed);
    g_touch_y.store(y, std::memory_order_relaxed);
    const bool was_pressed = g_touch_pressed.exchange(pressed, std::memory_order_relaxed);
    g_touch_valid.store(true, std::memory_order_relaxed);
    touch_pressed_ = pressed;
    // 只在状态跳变时打印，按住期间的重复喂点不打（否则每秒几十行）。
    if (pressed != was_pressed) {
        ESP_LOGW(TAG, "FEED #%u %s->%s (%d,%d)",
                 static_cast<unsigned>(g_feed_count.fetch_add(1) + 1),
                 was_pressed ? "P" : "R", pressed ? "P" : "R",
                 static_cast<int>(x), static_cast<int>(y));
    }

    // 状态跳变（按下 or 松开）压入边沿队列。
    // 目的：LVGL 任务两次唤醒之间可能合并掉一次跳变，导致"只读到松开、
    // 没读到按下"，CLICKED 永远不成立。压队列后每个跳变各保证被读一次。
    if (pressed != was_pressed) {
        const int tail = g_edge_tail.load(std::memory_order_relaxed);
        const int next = (tail + 1) % kEdgeQueueDepth;
        if (next != g_edge_head.load(std::memory_order_acquire)) {
            TouchEdge e;
            e.valid = true;
            e.pressed = pressed;
            e.x = x;
            e.y = y;
            g_edge_q[tail].store(e, std::memory_order_release);
            g_edge_tail.store(next, std::memory_order_release);
        } else {
            // 队列满（读写都在同一轮里被卡住，极罕见）：丢弃最老的一条，
            // 保证最新的跳变不丢 —— 最新状态对用户更重要。
            TouchEdge e;
            e.valid = true;
            e.pressed = pressed;
            e.x = x;
            e.y = y;
            g_edge_q[tail].store(e, std::memory_order_release);
            g_edge_tail.store(next, std::memory_order_release);
            g_edge_head.store((g_edge_head.load(std::memory_order_acquire) + 1) % kEdgeQueueDepth,
                              std::memory_order_release);
        }
    }

    // 关键：本工程实际用的是 esp_lvgl_port 的 LVGL9 分支（managed_components/
    // lvgl__lvgl 是 9.3.0）。LVGL9 下 lv_indev_read() 【不会】由 lv_timer_handler
    // 的 indev read_timer 驱动 —— lvgl_port_task 只在收到 LVGL_PORT_EVENT_TOUCH
    // 事件位时才显式调 lv_indev_read()（见 src/lvgl9/esp_lvgl_port.c:224）。
    //
    // 因此这里【每一次】喂点都要唤醒，包括"保持"和"松开"：
    //   * 按下时唤醒 -> LVGL 读到 PRESSED，派发 PRESSED；
    //   * 松开时唤醒 -> LVGL 读到 RELEASED，派发 RELEASED + CLICKED。
    // 早先只在按下时唤醒（或干脆不唤醒）会导致松开事件永远读不到，
    // CLICKED 也就永远不成立 —— 那正是"点一次没反应"的根源。
    lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, nullptr);

}

// ----------------------------------------------------------------------------
// 后端切换（调用方必须已持有 LVGL 锁）
// ----------------------------------------------------------------------------
void PageManager::SwitchToLvgl()
{
    if (in_lvgl_) {
        return;
    }
    // 顺序很重要：先把"归属"切到 LVGL，再让 gfx 静默。
    // 若反过来，gfx 在静默生效前发起的那笔传输完成时会被误判成 LVGL 的。
    // gfx 静默后它不会再碰 SPI，所以此后只有 LVGL 会写 panel。
    if (gfx_mute_ != nullptr) {
        gfx_mute_(gfx_mute_ctx_, true);
    }
    // 启动 LVGL 的 tick（lvtimer 定时器），LVGL 任务才会跑 lv_timer_handler。
    if (lvgl_ready_ && !lvgl_ticking_) {
        const esp_err_t err = lvgl_port_resume();
        lvgl_ticking_ = (err == ESP_OK);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "lvgl_port_resume failed: %s", esp_err_to_name(err));
        }
    }
    in_lvgl_ = true;
    ESP_LOGW(TAG, ">>> backend = LVGL (gfx muted)");
}

void PageManager::SwitchToGfx()
{
    if (!in_lvgl_) {
        return;
    }
    // 2) 先把归属置成"无人认领"，再停 LVGL。
    //    这样 LVGL 还在途的那一笔 DMA 完成时会被 ISR 直接丢弃；
    //    如果等到下面再把归属改成 GFX，这笔在途传输就会被误判成 gfx 的，
    //    而此刻 gfx 侧回调槽里的 user_ctx 可能已经不稳（引擎重建中）=> panic。
    emote::GfxSetFlushOwnerNone();
    if (lvgl_ticking_) {
        lvgl_port_stop();
        lvgl_ticking_ = false;
    }
    // 2.5) 屏障：等 LVGL 真正"停手"。
    //
    // 为什么必须等，这里踩过的坑（表现为"表情界面偏移 / 画面丢失一些东西"）：
    //   * lvgl_port_stop() 只做 lv_timer_enable(false) + esp_timer_stop(tick)。
    //     它既不打断 lvgl 任务里正在跑的 lv_timer_handler()，
    //     也不打断已经进入 wait_for_flushing 的那一帧 —— 那一帧会继续把
    //     余下的若干块 DMA 全部发完（360 行 = 9 块 × 40 行）。
    //   * 而下面 gfx_mute_(false) 要花 ~1.8ms 重建引擎，期间 gfx 的
    //     gfx_core_task 立刻开始整屏重刷（gfx_object_handler 在 child_list
    //     非空时无条件返回 true）。
    //   * 于是两个后端在同一个 SPI 上交错写：LVGL 的 40 行块和 gfx 的 16 行块
    //     互相覆盖，屏幕内容就错位 —— 这就是"偏移"。
    //
    // 做法：持有 LVGL 锁（阻止新的 flush_cb 进入）后，把在途传输计数等到归零。
    // 注意不能只等 g_spi_inflight 因为这个计数是 LVGL 在 flush_cb 里加的，
    // 而 flush_cb 本身在 LVGL 锁内 —— 先拿锁再等计数，顺序才对。
    if (lvgl_port_lock(kLvglLockTimeoutMs)) {
        // g_spi_inflight 归零即代表"最后一笔 DMA 的完成回调已经跑完"。
        // 等待有上限（kDrainTimeoutMs），超时也放行，只是记一笔告警。
        const int64_t t0 = esp_timer_get_time();
        while (!emote::GfxSpiIdle()) {
            if (esp_timer_get_time() - t0 > kDrainTimeoutMs * 1000) {
                ESP_LOGW(TAG, "SwitchToGfx: drain timeout, in-flight dma still pending");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "SwitchToGfx: lvgl lock timeout before drain");
    }
    // 3) 恢复 gfx。SetDesktopActive(false) 内部会重建引擎，
    //    构造完成后它自己会把归属写回 GFX（回调已重新登记）。
    if (gfx_mute_ != nullptr) {
        gfx_mute_(gfx_mute_ctx_, false);
    }
    // 兜底：若重建失败（gfx_mute_ 为空或引擎没建成），归属仍是 None，
    // 这里强制回到 GFX，避免表情页永远收不到 flush_ready。
    emote::GfxSetFlushOwnerGfx();
    in_lvgl_ = false;
    ESP_LOGW(TAG, "<<< backend = GFX (emote resumed), lvgl frames=%u px=%u err=%u wait_to=%u",
             static_cast<unsigned>(g_flush_blocks.exchange(0)),
             static_cast<unsigned>(g_flush_px.exchange(0)),
             static_cast<unsigned>(g_flush_errors.exchange(0)),
             static_cast<unsigned>(g_flush_timeouts.exchange(0)));
    g_first_flush = true;
}

bool PageManager::GotoPage(UiPage* next)
{
    if (next == nullptr) {
        return false;
    }
    // 页面切换会改 LVGL 对象树（OnExit / OnEnter / invalidate），必须持 LVGL 锁。
    // lvgl_port 的锁是递归互斥量，HandleSwipe 已持锁时再进一次是安全的。
    if (!lvgl_port_lock(kLvglLockTimeoutMs)) {
        ESP_LOGE(TAG, "GotoPage: lvgl_port_lock timeout");
        return false;
    }
    const bool ok = GotoPageLocked(next);
    lvgl_port_unlock();
    return ok;
}

bool PageManager::GotoPageLocked(UiPage* next)
{
    if (current_ == next) {
        // 即便同页也要保证后端正确（例如冷启动后第一次进 LVGL）
        if (next->Backend() == UIBackend::LVGL && !in_lvgl_) {
            SwitchToLvgl();
            // 必须补一次 OnEnter：BackToEmote() 会把 current_ 复位成 launcher_，
            // 同时 launcher 的 OnExit() 已经把自己 HIDDEN 掉了。这里若只切后端
            // 不重新 OnEnter，页面仍是隐藏状态 —— LVGL 判定没有脏区，
            // 一帧都不刷，表现为"左滑后黑屏"（也就是 frames=0）。
            next->OnEnter();
            if (lv_root_ != nullptr) {
                lv_obj_invalidate(lv_root_);
            }
        }
        return true;
    }

    UiPage* prev = current_;

    // 时序：先把新页面的后端接上，再调 OnEnter —— 这样页面在 OnEnter 里
    // 创建控件时不依赖 LVGL 任务已经在跑（LVGL 只要求对象树可用）。
    // 但 LVGL 任务必须在 SwitchToLvgl 之前 resume，否则首帧不会刷出来，
    // 所以这里用 "切后端 → OnEnter → 强制刷新" 的顺序。
    if (next->Backend() == UIBackend::LVGL) {
        SwitchToLvgl();
    } else {
        SwitchToGfx();
    }

    // 旧页面在切换后端之后再 OnExit，避免它在已经不属于它的后端上操作
    if (prev != nullptr) {
        prev->OnExit();
    }

    current_ = next;
    next->OnEnter();

    // OnEnter 里新建的对象不会自动出第一帧：LVGL 只在对象 dirty 时刷新，
    // 而新页面可能是把内容画在根对象之外的临时对象上，这里主动把根对象
    // 标脏一次，保证进入即可见（使用缓存的 lv_root_，避免依赖 lv_screen_active）。
    if (next->Backend() == UIBackend::LVGL && lv_root_ != nullptr) {
        lv_obj_invalidate(lv_root_);
    }

    ESP_LOGW(TAG, "page %s -> %s", prev ? prev->Name() : "(none)", next->Name());
    // 导航语义自检：把"当前处在哪一层"打出来，方便对照预期流程
    //   表情页 --左滑--> 桌面 --点击--> Timer --点击--> 模拟钟
    //   --右滑--> 桌面 --右滑--> 表情页
    ESP_LOGW(TAG, "nav: in_lvgl=%d cur=%s (back-右滑 will go to %s)",
             (int)in_lvgl_, current_ ? current_->Name() : "(none)",
             (current_ == launcher_) ? "emote" : "launcher");
    return true;
}

// ----------------------------------------------------------------------------
// 对外：手势 / 点击
// ----------------------------------------------------------------------------
void PageManager::HandleSwipe(SwipeDir dir)
{
    if (!inited_ || mutex_ == nullptr) {
        return;
    }
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(kMutexTimeoutMs)) != pdTRUE) {
        ESP_LOGW(TAG, "HandleSwipe: mutex timeout");
        return;
    }

    // 锁顺序：先拿本类 mutex，再拿 LVGL 锁（lv_display_create / lv_obj_* 都
    // 必须在持 LVGL 锁时调用，否则会与 LVGL 任务里的 lv_timer_handler 竞态）。
    //
    // 关键约束：本函数运行在 EchoEar 的 touch_task（优先级 5）里。
    // 历史事故——用 kLvglLockTimeoutMs=1000 等锁时，touch_task 把 CPU 占满
    // 一整秒，优先级更低的音频读线程完全拿不到 CPU，直接导致：
    //     E i2s_common: i2s_channel_read: The channel is not enabled
    //     W AFE: Ringbuffer of AFE(FEED) is full
    // 所以这里改成【零超时】：抢不到就丢弃这一次手势。
    // 手势是高频的瞬时事件，丢一次用户重划一下即可；
    // 而阻塞一秒会把语音链路直接打断，代价高得多。
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(TAG, "HandleSwipe: lvgl busy, drop this swipe");
        xSemaphoreGive(mutex_);
        return;
    }

    const bool need_lvgl = (dir == SwipeDir::LEFT) && !in_lvgl_;
    if (need_lvgl && !EnsureLvglDisplay()) {
        ESP_LOGE(TAG, "HandleSwipe: LVGL not available, stay on emote");
        lvgl_port_unlock();
        xSemaphoreGive(mutex_);
        return;
    }

    // 先记录进入本函数前的模式。
    // 判分支必须用"进入前"的状态：GotoPage(launcher_) 会把 in_lvgl_ 改成 true，
    // 若之后再用 in_lvgl_ 判断，第一次左滑会被误判成"已在桌面内"。
    const bool was_in_lvgl = in_lvgl_;

    bool handled = false;
    if (was_in_lvgl && current_ != nullptr) {
        // 已经在 LVGL 里：先让当前页面自己消化手势（比如 app 内部的横滑）
        handled = current_->OnGesture(dir);
    }
    if (!handled) {
        if (dir == SwipeDir::LEFT) {
            if (!was_in_lvgl) {
                // 表情页 --左滑--> 桌面。这是唯一允许从 gfx 跨到 LVGL 的入口。
                handled = GotoPage(launcher_);
            } else if (current_ != launcher_) {
                // 已在桌面内：左滑回到桌面（等价于返回上一层的另一方向）
                handled = BackToLauncher();
            }
        } else {
            // 右滑逐层返回：app → 桌面 → 表情页
            if (!was_in_lvgl) {
                // 表情页右滑：没有上一层，忽略
                handled = false;
            } else if (current_ == launcher_ || current_ == nullptr) {
                handled = BackToEmote();
            } else {
                handled = BackToLauncher();
            }
        }
    }

    lvgl_port_unlock();
    xSemaphoreGive(mutex_);

    if (!handled) {
        ESP_LOGW(TAG, "HandleSwipe(%s) not handled (in_lvgl=%d)",
                 dir == SwipeDir::LEFT ? "LEFT" : "RIGHT", (int)was_in_lvgl);
    }
}

// ----------------------------------------------------------------------------
// 导航语义：逐层返回（左滑进桌面 → 右滑逐层退：app → 桌面 → 表情）
// ----------------------------------------------------------------------------
bool PageManager::OpenApp(UiPage* app)
{
    if (app == nullptr) {
        return false;
    }
    // 允许从 gfx 直接进某个 LVGL 页面（首次左滑进桌面就走这条）。
    // 建屏必须在 LVGL 锁内完成：lv_display_create / lv_obj_* 都会改
    // LVGL 的全局链表，与 LVGL 任务里的 lv_timer_handler 竞态会直接跑飞。
    // 因此这里先拿锁 → 按需建屏 → 释放锁 → 再走 GotoPage（它内部自己加锁）。
    if (app->Backend() == UIBackend::LVGL && lv_disp_ == nullptr) {
        if (!lvgl_port_lock(kLvglLockTimeoutMs)) {
            ESP_LOGE(TAG, "OpenApp(%s): lvgl_port_lock timeout", app->Name());
            return false;
        }
        const bool ok = EnsureLvglDisplay();
        lvgl_port_unlock();
        if (!ok) {
            ESP_LOGE(TAG, "OpenApp(%s): LVGL display unavailable", app->Name());
            return false;
        }
    }
    return GotoPage(app);
}

bool PageManager::BackToLauncher()
{
    if (launcher_ == nullptr || current_ == launcher_) {
        return false;
    }
    return GotoPage(launcher_);
}

bool PageManager::BackToEmote()
{
    if (!in_lvgl_) {
        return false;
    }
    // OnExit() 要碰 LVGL 对象（隐藏 / 删 timer），必须持 LVGL 锁。
    if (!lvgl_port_lock(kLvglLockTimeoutMs)) {
        ESP_LOGE(TAG, "BackToEmote: lvgl_port_lock timeout");
        return false;
    }
    UiPage* prev = current_;
    // 先 stop LVGL 再恢复 gfx（SwitchToGfx 内部已保证该顺序）
    SwitchToGfx();
    if (prev != nullptr) {
        prev->OnExit();
    }
    // current_ 回到 launcher：下次左滑进桌面时直接显示图标表，
    // 而不是停留在上次打开的那个 app 上。
    current_ = launcher_;
    lvgl_port_unlock();
    return true;
}

}  // namespace coze_ui
