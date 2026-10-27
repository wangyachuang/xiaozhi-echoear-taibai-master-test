#pragma once

#include "display.h"
#include "lvgl_font.h"
#include <memory>
#include <functional>
#include <map>
#include <string>
#include <vector>   // LayoutRecord 缓存（引擎重建后重放 layout.json 的配置）
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

namespace emote {

// ----------------------------------------------------------------------------
// panel 传输完成的分发接口（gfx ⇄ LVGL 分时复用单槽回调）
// ----------------------------------------------------------------------------
// 这块屏的 panel_io 只有一个 on_color_trans_done 槽位。esp_lvgl_port 的
// lvgl_port_add_disp() 会无条件重注册并顶掉 gfx 的回调，顶掉后 gfx 的
// flush_ready 永远等不到（表现为表情一帧一帧卡死）。所以现在约定：
//
//   **回调槽位永远只属于 gfx（EmoteEngine::OnFlushIoReady），谁都不许重注册。**
//
// 谁发起 SPI 传输，谁就在发起前声明归属；ISR 里按归属把"完成事件"
// 路由回真正发起的那一方：
//      gfx  : 内部在 OnFlush 里自动置位，无需上层干预
//      LVGL : 调用 GfxSetFlushOwnerLvgl() 声明，再由 GfxNotifyLvglFlushDone()
//             在 ISR 中被回调 ← 这就是 LVGL 侧的 flush_ready 来源
//
// 这样 esp_lcd_panel_io_register_event_callbacks 全程只被调用一次。
void GfxSetFlushOwnerLvgl();                                  // 发起 LVGL 传输前调用
void GfxSetFlushOwnerGfx();                                   // 归还给 gfx（可省略，OnFlush 会自动置位）
// 引擎销毁/重建的窗口期把归属置为"无人认领"：
// 此时 panel_io 槽里的 user_ctx 可能已是被释放的旧句柄，ISR 必须丢弃完成事件
// 而不能解引用。用法：
//   SetDesktopActive(true)  -> 先 GfxSetFlushOwnerNone()，再销毁引擎
//   SetDesktopActive(false) -> 新引擎构造完成（回调已重登记）后再 GfxSetFlushOwnerGfx()
void GfxSetFlushOwnerNone();
bool GfxFlushOwnerIsNone();                                   // 当前是否处于"无人认领"窗口
// 在途 SPI 传输记账：**任何**发起 draw_bitmap 的一方（gfx 内部已自动调用，
// LVGL 侧必须自己调用）都要在发起前 +1。ISR 端 OnFlushIoReady 统一 -1。
// 只有 gfx 与 LVGL 两边都记账，销毁引擎前的"等总线空闲"才是准的；
// 漏记会让计数变负、drain 判断失效，从而漏掉残留的 flush_ready 命令。
void GfxSpiInflightAdd();                                     // 发起传输前调用
// 任务上下文里的回滚（例如 draw_bitmap 直接失败、中断根本不会来）。
// 正常完成路径由 ISR 统一 -1，调用方不要重复调用。
void GfxSpiInflightDone();
// 总线是否已彻底空闲（没有在途 DMA）。
// 用于"把面板从一端交给另一端"之前的屏障：例如 LVGL 交回 gfx 时，
// 必须先确认 LVGL 那几块 40 行的 DMA 全部落地，否则 gfx 重建期间
// 两边交错写 SPI，画面会出现错位/残留（表现为"表情界面偏移"）。
bool GfxSpiIdle();
using LvglFlushDoneFn = void (*)(void);                       // 在 ISR 上下文执行
void GfxSetLvglFlushDoneHandler(LvglFlushDoneFn fn);          // 注册 LVGL 侧的完成处理
void GfxNotifyLvglFlushDone();                                // 由 ISR 内部调用

// Simple data structure for storing asset data without LVGL dependency
struct AssetData {
    const void* data;
    size_t size;
    union {
        uint8_t flags;  // 1 byte for all animation flags
        struct {
            uint8_t fps : 6;    // FPS (0-63) - 6 bits
            uint8_t loop : 1;   // Loop animation - 1 bit
            uint8_t lack : 1;   // Lack animation - 1 bit
        };
    };

    AssetData() : data(nullptr), size(0), flags(0) {}
    AssetData(const void* d, size_t s) : data(d), size(s), flags(0) {}
    AssetData(const void* d, size_t s, uint8_t f, bool l, bool k)
        : data(d), size(s)
    {
        fps = f > 63 ? 63 : f;  // 限制 FPS 到 6 位范围
        loop = l;
        lack = k;
    }
};

// Layout element data structure
struct LayoutData {
    char align;  // Store as char instead of string
    int x;
    int y;
    int width;
    int height;
    bool has_size;

    LayoutData() : align(0), x(0), y(0), width(0), height(0), has_size(false) {}
    LayoutData(char a, int x_pos, int y_pos, int w = 0, int h = 0)
        : align(a), x(x_pos), y(y_pos), width(w), height(h), has_size(w > 0 && h > 0) {}
};

// Function to convert align string to GFX_ALIGN enum value
char StringToGfxAlign(const std::string &align_str);

class EmoteEngine;

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

    void AddEmojiData(const std::string &name, const void* data, size_t size, uint8_t fps = 0, bool loop = false, bool lack = false);
    void AddIconData(const std::string &name, const void* data, size_t size);
    void AddLayoutData(const std::string &name, const std::string &align_str, int x, int y, int width = 0, int height = 0);
    void AddTextFont(std::shared_ptr<LvglFont> text_font);
    AssetData GetEmojiData(const std::string &name) const;
    AssetData GetIconData(const std::string &name) const;

    EmoteEngine* GetEngine() const;
    void* GetEngineHandle() const;

    // ------------------------------------------------------------------
    // 引擎让路接口（供 LVGL 页面在共享 panel 上切换后端使用）
    //
    // 背景：gfx 与 esp_lvgl_port 共用 panel_io 的 on_color_trans_done，
    // 且 gfx_core.c 的 gfx_object_handler() 末尾无条件 updated=true，
    // 所以只要对象树非空，gfx 每轮都会整屏重绘。要真正让出屏幕，
    // 只能替换 gfx 的 flush_cb。
    //
    // SetGfxFlushOverride(nullptr) → 恢复真实 flush（表情正常播放）
    // SetGfxFlushOverride(cb)      → gfx 只在内部空转，不碰 SPI
    // ------------------------------------------------------------------
    using GfxFlushFn = void (*)(void* handle, int x1, int y1, int x2, int y2, const void* data);
    void SetGfxFlushOverride(GfxFlushFn flush);

    // ------------------------------------------------------------------
    // panel_io 完成回调的归属仲裁（gfx 与 LVGL 共用同一个槽位）
    //
    // 背景：这块屏的 panel_io 只有一个 on_color_trans_done 槽位。
    // 历史事故——LVGL 侧的 esp_lvgl_port 会无条件重注册该槽位并把 gfx 顶掉，
    // 导致 gfx 的 flush_ready 永远等不到，表情一帧一帧卡死。
    //
    // 现在的约定：
    //   * 槽位永远只属于 gfx（EmoteEngine::OnFlushIoReady），任何模块都不再重注册；
    //   * 谁发起 SPI 传输，谁就在 draw_bitmap 之前调用 SetFlushOwner 声明归属；
    //   * ISR 里按归属把完成事件路由给真正的发起方。
    // ------------------------------------------------------------------
    // 注意：SetFlushOwner 一族和 GfxSetLvglFlushDoneHandler 都是
    // **namespace emote 的自由函数**（见本文件顶部），不要在这里再声明一份
    // 同名成员。曾经在这里留过只有声明、没有定义的成员版本，结果类内调用时
    // 名称查找优先命中成员，链接期报
    // undefined reference to `emote::EmoteDisplay::GfxSetFlushOwnerGfx()'。

    // 提供 panel_io，供切换后端时重新注册完成回调
    void SetPanelIo(esp_lcd_panel_io_handle_t panel_io) { panel_io_ = panel_io; }
    esp_lcd_panel_io_handle_t GetPanelIo() const { return panel_io_; }

    inline std::shared_ptr<LvglFont> text_font() const
    {
        return text_font_;
    }

    // ------------------------------------------------------------------
    // 桌面（LVGL 页面）占屏开关
    //
    // SetDesktopActive(true)  → 表情闸门关闭：SetEmotion / SetStatus /
    //     SetChatMessage / ShowNotification / UpdateStatusBar 全部直接返回，
    //     不再给 gfx 对象打任何脏标记，gfx 任务彻底空转。
    // SetDesktopActive(false) → 重新按 g_last_emotion / g_current_icon_type
    //     恢复表情页现场。
    //
    // 之所以由 EmoteDisplay 自己实现而不是靠 flush 转发：gfx_core 只要对象树
    // 非空就每轮整屏重绘，转发 flush 只能挡住 SPI 写，挡不住 CPU 消耗，
    // 而且会打乱 flush_ready 的计数。
    // ------------------------------------------------------------------
    void SetDesktopActive(bool active);

    // 供 Timer 等 LVGL 应用读取电量（百分比），-1 = 未知。
    // 内部带 5s 节流，不会每次调用都走 I2C。
    int GetBatteryPercent();

private:
    void InitializeEngine(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    std::unique_ptr<EmoteEngine> engine_;

    // 后端切换用：flush 覆盖 + panel_io 记录
    // （覆盖指针存在 emote_display.cc 的转发表里，见 SetGfxFlushOverride）
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;

    // 引擎重建所需的面板句柄与分辨率。
    // SetDesktopActive(true) 会销毁 engine_ 以彻底让出 SPI，
    // SetDesktopActive(false) 用这三项把引擎重新构造出来。
    esp_lcd_panel_handle_t panel_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    // layout.json 里的布局记录，用于【引擎重建后重放】。
    //
    // 为什么必须缓存：AddLayoutData() 由 main/assets.cc 在启动时调用一次，
    // 之后再也不会有第二次。而桌面切换会销毁并重建整个 gfx 引擎（对象全部新建），
    // 重建后若只靠 SetupUI() 里写死的常量，就会与 layout.json 的实际配置脱节 ——
    // 这正是"左滑进桌面再滑回来，电池图标跑到左上角"的根因。
    // 这里把每次收到的布局存下来，重建完成后按原顺序重新 align 一遍，
    // 保证「启动时」和「重建后」两种状态的布局完全一致。
    struct LayoutRecord {
        std::string name;
        std::string align;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };
    std::vector<LayoutRecord> layouts_;
    void ReplayLayouts();   // 引擎重建后重新应用 layouts_（调用方须已持锁）

    // 引擎生命周期锁（递归互斥量）。
    // 与 gfx 内部的锁无关，独立存在，用于保证"销毁/重建 engine_"
    // 与"其他任务使用 engine_"之间的互斥，避免 use-after-free。
    SemaphoreHandle_t engine_mutex_ = nullptr;

    // 记录当前嵌套层里真正拿到 gfx 渲染锁的层数，供 Unlock() 精确配对。
    // 必须是计数而不是 bool：嵌套 Lock 时内层 Unlock 会把 bool 清掉，
    // 导致外层 Unlock 漏掉一次 gfx_emote_unlock（锁泄漏 -> 下次卡死）。
    int gfx_lock_depth_ = 0;

    // Font management
    std::shared_ptr<LvglFont> text_font_ = nullptr;

    // Non-LVGL asset data storage
    std::map<std::string, AssetData> emoji_data_map_;
    std::map<std::string, AssetData> icon_data_map_;

};

} // namespace emote
