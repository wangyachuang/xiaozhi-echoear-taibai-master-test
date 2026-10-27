// ============================================================================
// lvgl_clock_probe.cc — 第 1 步验证：panel IO 回调切换架构
// ============================================================================
#include "lvgl_clock_probe.h"

#if BOARD_LVGL_CLOCK_PROBE

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <cstring>

#define TAG "LVGL_CLOCK_PROBE"

static const char* TAG_MAIN = "LVGL_CLOCK_PROBE";

// ---- 保存平台句柄 ----
static esp_lcd_panel_io_handle_t s_panel_io = nullptr;
static esp_lcd_panel_handle_t    s_panel    = nullptr;
static lv_display_t*             s_disp     = nullptr;
static bool                      s_inited   = false;
static bool                      s_in_lvgl  = false;   // 当前是否处于 LVGL 模式

// gfx 侧的回调注册函数（由 EmoteDisplay 注入）
static void (*s_gfx_registrar)(void*) = nullptr;
static void*  s_gfx_engine_handle     = nullptr;

// LVGL 的 UI 根对象
static lv_obj_t* s_scr = nullptr;
static lv_timer_t* s_timer = nullptr;

// ----------------------------------------------------------------------------
// 统计：分别记录两种模式下各自刷新的次数，便于判断回调切换是否真的生效
// ----------------------------------------------------------------------------
static volatile uint32_t s_lvgl_flush_cnt = 0;

// LVGL 完成回调由 esp_lvgl_port 内部注册，这里不需要自己处理；
// 但为了验证"切换是否生效"，我们额外记录一次刷新发生。
static void probe_timer_cb(lv_timer_t* t)
{
    lv_obj_t* arc = (lv_obj_t*)lv_timer_get_user_data(t);
    if (arc == nullptr) {
        return;
    }
    // 让圆弧缓慢旋转，持续制造脏区域（验证 LVGL 能在该 panel 上正常刷）
    static int32_t rot = 0;
    rot = (rot + 15) % 360;
    lv_arc_set_rotation(arc, rot);
    s_lvgl_flush_cnt++;
}

// ----------------------------------------------------------------------------
// 创建测试界面：一个青色圆弧 + 一行文字
// ----------------------------------------------------------------------------
static void build_test_ui(void)
{
    if (s_disp == nullptr) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock 失败，无法创建 UI");
        return;
    }

    s_scr = lv_display_get_screen_active(s_disp);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101820), LV_PART_MAIN);

    // 青色进度弧（模拟时钟页将来的"进度环"）
    lv_obj_t* arc = lv_arc_create(s_scr);
    lv_obj_set_size(arc, 260, 260);
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 65);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x2A3442), LV_PART_MAIN);      // 底环
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x00C8FF), LV_PART_INDICATOR); // 进度
    // 去掉旋钮，只留弧（注意 LVGL 9 用 remove_flag，不是 LVGL 8 的 clear_flag）
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    // 文字（验证 label / 字体路径）
    lv_obj_t* lbl = lv_label_create(s_scr);
    lv_label_set_text(lbl, "LVGL CLOCK PROBE");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    lvgl_port_unlock();

    // 每秒转一次，制造持续刷新
    s_timer = lv_timer_create(probe_timer_cb, 1000, arc);

    ESP_LOGW(TAG_MAIN, "测试界面已创建：中央青色圆弧 + 文字");
}

// ----------------------------------------------------------------------------
// 公开接口
// ----------------------------------------------------------------------------
void LvglClockProbe_SetGfxCallbackRegistrar(void (*fn)(void*), void* engine_handle)
{
    s_gfx_registrar     = fn;
    s_gfx_engine_handle = engine_handle;
    ESP_LOGI(TAG, "已注入 gfx 回调注册器: fn=%p handle=%p", (void*)fn, engine_handle);
}

void LvglClockProbe_Init(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel)
{
    if (s_inited) {
        ESP_LOGW(TAG, "已初始化，跳过");
        return;
    }
    s_panel_io = panel_io;
    s_panel    = panel;

    ESP_LOGW(TAG, "=========== LVGL 时钟页探测（第 1 步）初始化 ===========");

    // ---- 1) LVGL 库初始化 ----
    // 注意：EchoEar 走 EmoteDisplay 分支，此前从未调用 lv_init()。
    lv_init();
    ESP_LOGW(TAG, "[1/3] lv_init() 完成");

    // ---- 2) LVGL port 初始化（创建 LVGL 任务 + 心跳）----
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;   // 与 lcd_display.cc 一致
    port_cfg.task_affinity = 1;   // 放核心 1，尽量避开 gfx（核心 0）
    esp_err_t perr = lvgl_port_init(&port_cfg);
    ESP_LOGW(TAG, "[2/3] lvgl_port_init -> %s", esp_err_to_name(perr));
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port 初始化失败，探测终止");
        return;
    }

    // ---- 3) 把 LVGL 挂到同一个 panel 上 ----
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(360 * 40),
        .double_buffer = false,
        .trans_size = 0,
        .hres = 360,
        .vres = 360,
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
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "[3/3] lvgl_port_add_disp 失败");
        return;
    }
    ESP_LOGW(TAG, "[3/3] lvgl_port_add_disp 成功");

    build_test_ui();
    s_inited = true;
    ESP_LOGW(TAG, "初始化完成。注意：此刻 LVGL 已抢走 panel 回调，"
                  "gfx 的回调被覆盖，表情页会卡（预期现象，Leave() 后恢复）");
}

void LvglClockProbe_Enter(void)
{
    if (!s_inited || s_in_lvgl) {
        return;
    }
    ESP_LOGW(TAG, ">>> Enter(): 切换到 LVGL 绘制（模拟进入时钟页）");

    // 说明：LVGL 的回调在 lvgl_port_add_disp 时已注册。
    // 这里只需要把 LVGL 的界面显示出来；gfx 的回调已被覆盖。
    if (s_scr) {
        if (lvgl_port_lock(0)) {
            lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_scr);
            lvgl_port_unlock();
        }
    }
    if (s_timer) {
        lv_timer_resume(s_timer);
    }
    s_in_lvgl = true;
    ESP_LOGW(TAG, "    当前刷新计数: LVGL=%u", (unsigned)s_lvgl_flush_cnt);
}

void LvglClockProbe_Leave(void)
{
    if (!s_inited || !s_in_lvgl) {
        return;
    }
    ESP_LOGW(TAG, "<<< Leave(): 切回 gfx 绘制（模拟离开时钟页）");

    // 1) 停掉 LVGL 的持续刷新，避免它继续与 gfx 抢总线
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
    // 2) 隐藏 LVGL 界面
    if (s_scr) {
        if (lvgl_port_lock(0)) {
            lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
    }

    // 3) ★ 核心验证点：把 panel IO 的完成回调重新注册回 gfx 引擎。
    //    若这一步有效，表情页应立刻恢复正常（不再等 50ms 超时）。
    if (s_gfx_registrar && s_gfx_engine_handle) {
        s_gfx_registrar(s_gfx_engine_handle);
        ESP_LOGW(TAG, "    已重新注册 gfx 回调（engine=%p）", s_gfx_engine_handle);
    } else {
        ESP_LOGE(TAG, "    未注入 gfx 回调注册器！表情页将无法恢复 —— "
                      "请确认 EmoteDisplay 调用了 SetGfxCallbackRegistrar()");
    }

    s_in_lvgl = false;
    ESP_LOGW(TAG, "    LVGL 累计刷新=%u", (unsigned)s_lvgl_flush_cnt);
}

#endif  // BOARD_LVGL_CLOCK_PROBE
