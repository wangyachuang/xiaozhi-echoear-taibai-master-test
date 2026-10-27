#pragma once

// ---------------------------------------------------------------------------
// clock_view.h — 时钟页 UI（数字时钟 / 模拟时钟，点击切换）
//
// 设计要点（对应参考图 UI）：
//   * 数字时钟页：顶部电池百分比 + 日期行 + 中央大号 HH:MM（青/红）+
//     绿色进度弧（带弧排 "DAILY MISSION"）+ 完成度 % + 步数。
//   * 模拟时钟页：顶部 86% + 小号 HH:MM + 中央时钟表盘（时/分/秒三指针）+
//     右侧日期 + 左下进度弧/完成度/步数。
//   * 静态内容与动态内容分离，避免整屏重复刷新：
//       - 所有文字均为 gfx label，各自去重（文本未变不调用 set_text）。
//       - 进度弧只在百分比变化时重绘 + set_src。
//       - 模拟指针图层只在"秒"变化时重绘，且图层尺寸仅 kHandsLayerSize
//         （默认 140x140），而不是全屏 360x360 —— 每秒重绘像素数从
//         129,600 降到 19,600（约 1/6.6），PSRAM 缓冲从 388KB 降到 58.8KB。
//   * 表盘上静止的部分（刻度、圆心）绘制一次后不再触碰。
//
// 依赖关系（include 顺序有强制要求，勿随意调整）：
//   必须先包含 <lvgl.h>，再包含本头文件。
//   esp_emote_gfx 的兼容层 widget/gfx_font_lvgl.h 仅在 LVGL_VERSION_MAJOR
//   未定义时才自建一套 LVGL 8 类型副本；先包含真 LVGL(9.3) 可让它直接沿用
//   真实类型，否则所有 LVGL 字体/图像类型会被声明两份，编译必然冲突。
//   emote_display.cc 的顺序同样是 emote_display.h -> lvgl_font.h -> <lvgl.h> 在前。
// ---------------------------------------------------------------------------

#include <lvgl.h>

#include <cstdint>

#include "gfx.h"

namespace clock_view {

// 布局常量（屏幕 360x360，见 boards/echoear/config.h）
constexpr int kScreenW = 360;
constexpr int kScreenH = 360;

// "DAILY MISSION" 字符数（含空格），Init/Update 均按此长度处理 label 数组
constexpr int kMissionCharCount = 13;

// ===========================================================================
// 模拟时钟指针图层边长（像素）—— ★ 可调，但必须与 clock_view.cc 的
//                                  kDialRadius 匹配，否则表盘会被裁掉。
// ---------------------------------------------------------------------------
// 硬约束： kHandsLayerSize >= 2 * kDialRadius + 边距(建议 ≥10)
//   例：kDialRadius = 64  -> kHandsLayerSize >= 138（当前取值 140，已满足）
//       kDialRadius = 70  -> kHandsLayerSize >= 150
//       kDialRadius = 80  -> kHandsLayerSize >= 170
// 缓冲占用 = kHandsLayerSize^2 * 3 字节（PSRAM），仅分配一次：
//   140 -> 58.8 KB      170 -> 86.7 KB      200 -> 120 KB
// 该图层就是"表盘+指针"整块位图，每秒钟只重绘它一次。
// ===========================================================================
constexpr int kHandsLayerSize = 140;

// 时钟页两种子界面
enum class ClockFace : uint8_t {
    DIGITAL = 0,  // 数字时钟（大号 HH:MM）
    ANALOG = 1,   // 模拟时钟（指针表盘）
};

// 初始化：创建时钟页所需的全部 gfx 对象（全部默认不可见）。
// 通过出参返回给调用方，EmoteDisplay 仍持有这些对象用于可见性控制。
//   out_battery_pct : 电池百分比文本（顶部，白色）
//   out_date        : 日期行文本（数字页："FRI 18 JUL 2025"，白色）
//   out_time_hh     : 时钟小时部分（数字页中央大号，青色）
//   out_time_mm     : 时钟分钟部分（数字页中央大号，红色）
//   out_time_colon  : 时钟冒号（数字页中央大号，青色）
//   out_steps_pct   : 步数完成度百分比（中下，绿色）
//   out_steps       : 步数（底部，白色）
//   out_mission     : 单字符 label 数组（长度 kMissionCharCount），弧排 "DAILY MISSION"
//   out_arc         : 进度弧（自绘图像，仅进度变化时重绘）
//   out_time_small  : 模拟页顶部小号 HH:MM（白色）
//   out_date_right  : 模拟页右侧日期（白色）
//   out_hands       : 模拟页指针图层（自绘图像，仅秒变化时重绘）
void Init(gfx_handle_t engine,
          gfx_obj_t*& out_battery_pct,
          gfx_obj_t*& out_date,
          gfx_obj_t*& out_time_hh,
          gfx_obj_t*& out_time_mm,
          gfx_obj_t*& out_time_colon,
          gfx_obj_t*& out_steps_pct,
          gfx_obj_t*& out_steps,
          gfx_obj_t** out_mission,
          gfx_obj_t*& out_arc,
          gfx_obj_t*& out_time_small,
          gfx_obj_t*& out_date_right,
          gfx_obj_t*& out_hands);

// 设置各文本 label 的字体（初始化后由 EmoteDisplay 调用一次）。
// big/mid/small 对应大号时钟、中等文本、小号弧排字符。
// mission 是长度为 kMissionCharCount 的 label 数组首地址。
void SetFonts(gfx_obj_t* label_battery_pct, gfx_obj_t* label_date,
              gfx_obj_t* label_time_hh, gfx_obj_t* label_time_mm, gfx_obj_t* label_time_colon,
              gfx_obj_t* label_steps_pct, gfx_obj_t* label_steps, gfx_obj_t** mission,
              gfx_obj_t* label_time_small, gfx_obj_t* label_date_right,
              void* big_font, void* mid_font, void* small_font);

// 切换数字 / 模拟时钟界面，返回切换后的界面。
// 由 EmoteDisplay::ToggleClockMode()（时钟页内点击）调用。
ClockFace ToggleFace();

// 当前界面。
ClockFace CurrentFace();

// 按当前 ClockFace 设置各对象的可见性。
// digital 元素组：date / hh / mm / colon
// analog  元素组：time_small / date_right / hands
// 两组共用的元素（battery_pct / steps_pct / steps / arc / mission）保持可见。
// 由 EmoteDisplay 在进入时钟页与切换界面时调用。
void ApplyFaceVisibility(gfx_obj_t* label_battery_pct,
                         gfx_obj_t* label_date,
                         gfx_obj_t* label_time_hh, gfx_obj_t* label_time_mm, gfx_obj_t* label_time_colon,
                         gfx_obj_t* label_steps_pct, gfx_obj_t* label_steps, gfx_obj_t** mission,
                         gfx_obj_t* arc,
                         gfx_obj_t* label_time_small, gfx_obj_t* label_date_right, gfx_obj_t* hands);

// 每秒刷新。内部按当前 ClockFace 只刷新该界面真正变化的字段：
//   * 数字页：小时/分钟/日期/电池/步数/进度弧，各自去重。
//   * 模拟页：仅指针图层（每秒一次）+ 小号时间文本（分钟变化时）+ 日期（跨天时）。
// 两个界面共用的静态字段（电池/步数/完成度/弧）同样各自去重，未变化时零打扰。
// mission 是长度为 kMissionCharCount 的 label 数组首地址
// （弧排字符内容与位置固定，Update 内不触碰，仅为接口完整性保留）。
// battery_value: 0-100；steps_value: 步数；steps_goal: 步数目标（用于完成度与弧）。
void Update(gfx_obj_t* label_battery_pct, gfx_obj_t* label_date,
            gfx_obj_t* label_time_hh, gfx_obj_t* label_time_mm, gfx_obj_t* label_time_colon,
            gfx_obj_t* label_steps_pct, gfx_obj_t* label_steps, gfx_obj_t** mission, gfx_obj_t* arc,
            gfx_obj_t* label_time_small, gfx_obj_t* label_date_right, gfx_obj_t* hands,
            int battery_value, int steps_value, int steps_goal);

// 进入时钟页时调用：重置去重缓存，强制下次全部刷新一次。
void OnPageEnter();

}  // namespace clock_view
