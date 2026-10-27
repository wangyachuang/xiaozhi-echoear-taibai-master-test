// ---------------------------------------------------------------------------
// clock_view.cc — 时钟页 UI 实现（数字时钟 / 模拟时钟，点击切换）
//
// 资源策略（设备资源有限，按"最省"方式实现）：
//   * 所有文字都是 gfx label，由引擎字体光栅化路径渲染，且逐字段去重：
//     文本未变则不调用 gfx_label_set_text，对引擎零打扰。
//   * 自绘位图只有两块：
//       - 进度弧 g_arc_buf：360x360 RGB565A8 仅分配 1 次；
//         只在进度百分比变化时重绘 + set_src（正常一分钟最多变一次）。
//       - 指针图层 g_hands_buf：仅 kHandsLayerSize(140) x 140；
//         只在"秒"变化时重绘（每秒 1 次），且表盘静态部分（刻度/圆心）
//         与动态部分（指针）分开：刻度每帧重画代价可控，
//         但为避免无谓开销，刻度用预生成的静态标记位直接置位，不做三角函数运算。
//   * 每秒重绘像素：数字页 ~0（只改变化的那一个 label）；
//     模拟页 140*140 = 19,600 像素（对比全屏 360x360 = 129,600 像素，约 1/6.6）。
//
// 依赖关系：
//   * <lvgl.h> 必须先于本文件包含（见 clock_view.h 的说明）。
//   * gfx.h / gfx_label.h / gfx_img.h 由 clock_view.h 带入。
//   * 字体通过 SetFonts 的 void* 参数注入，避免在本文件重复
//     LV_FONT_DECLARE 造成符号重复声明。
// ---------------------------------------------------------------------------

#include "clock_view.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gfx.h"

static const char* TAG = "ClockView";

namespace {

// ===========================================================================
// ★★★  可  调  参  数  区  ★★★
// ---------------------------------------------------------------------------
// 本文件所有可调项都集中在这里，改完只需重新编译 clock_view.cc。
// 坐标系约定（非常重要，先读这段再改数值）：
//   * 屏幕 360x360，原点 (0,0) 在【左上角】，x 向右、y 向下。
//   * 凡名字带 "Y" 且注释写"距顶部"的，都是【绝对像素】，0=屏幕最上沿。
//   * 凡用 GFX_ALIGN_CENTER 对齐的，给的 (x,y) 是相对【屏幕中心(180,180)】
//     的偏移：x 正数向右、y 正数向下。
//   * 指针图层内部的坐标以【图层中心】为原点，见"模拟表盘"一节。
// ===========================================================================

// ---------------------------------------------------------------------------
// 【一】通用 / 屏幕
// ---------------------------------------------------------------------------
constexpr int kScreenW = clock_view::kScreenW;   // 360，屏幕宽
constexpr int kScreenH = clock_view::kScreenH;   // 360，屏幕高

// ---------------------------------------------------------------------------
// 【二】顶部电池百分比 "86%"
//      两页（数字页 / 模拟页）共用同一个 label。
// ---------------------------------------------------------------------------
constexpr int kBatteryY = 30;                    // 距顶部像素，越小越靠上

// ---------------------------------------------------------------------------
// 【三】数字页（大号数字时钟）
// ---------------------------------------------------------------------------
constexpr int kDateY       = 118;   // 日期行 "FRI 18 JUL 2025" 距顶部像素
constexpr int kTimeOffsetY = -18;   // "19:42" 相对屏幕中心的垂直偏移，负数上移

// "19 : 42" 三段的水平间距（相对屏幕中心的偏移）。
// 想让数字更紧凑就减小绝对值，想更松散就增大。
// 注意：字号受内置字体限制（20px），改这三个值只挪位置、不改字大小。
constexpr int kTimeHhX    = -76;    // 小时 "19" 的 X（负=偏左）
constexpr int kTimeColonX = 0;      // 冒号 ":" 的 X（0=正中）
constexpr int kTimeMmX    = 76;     // 分钟 "42" 的 X（正=偏右）

// 数字页三段各自的 label 尺寸。
// ★ 重要（性能）：gfx 引擎对每个 label 都按其【包围盒 ∩ 当前块】逐像素混合，
//   即使实际字形很小。包围盒越大，整屏重绘越慢（实测一帧 47.8ms）。
//   因此这里只给"刚好容纳文字 + 少量余量"的尺寸，不要为了"留白"而放大。
//   参考：内置字体 20px，单个数字约 12x20、整串 "19" 约 26x22。
//   若改大字体或改文字内容，请相应调整这些值。
//   注意：label 尺寸不影响字形大小（字号由字体决定），只影响对齐与混合范围。
constexpr int kTimeHhW    = 64;     // 原为 140（过大）
constexpr int kTimeHhH    = 40;     // 原为 110（过大）
constexpr int kTimeColonW = 20;     // 冒号本身很窄
constexpr int kTimeColonH = 40;
constexpr int kTimeMmW    = 64;     // 原为 140
constexpr int kTimeMmH    = 40;     // 原为 110
constexpr int kDateW      = 200;    // "FRI 18 JUL 2025" 约 15 字符 x 12px
constexpr int kDateH      = 28;

// 步数完成度 "87%" 与步数 "1526"（两页共用）（距顶部像素）
constexpr int kStepsPctY  = 232;
constexpr int kStepsY     = 300;
constexpr int kStepsPctW  = 80;
constexpr int kStepsPctH  = 28;
constexpr int kStepsW     = 80;
constexpr int kStepsH     = 28;

// ---------------------------------------------------------------------------
// 【四】模拟页（指针表盘）
// ---------------------------------------------------------------------------
// 顶部小号时间 "19:42"
constexpr int kSmallTimeY = 92;      // 距顶部像素
constexpr int kSmallTimeW = 80;      // 原为 200（过大）
constexpr int kSmallTimeH = 28;      // 原为 36

// 右侧三行日期 "FRI / 18 JUL / 2025"
// 用 GFX_ALIGN_CENTER，所以是相对屏幕中心的偏移。
constexpr int kDateRightX = 74;      // 正数向右
constexpr int kDateRightY = 6;       // 正数向下
constexpr int kDateRightW = 110;     // "18 JUL" 约 6 字符
constexpr int kDateRightH = 76;      // 3 行 x 约 24px

// 顶部电池百分比 "86%"（两页共用）
constexpr int kBatteryW = 70;        // 原为 200（过大）
constexpr int kBatteryH = 28;

// 指针图层的中心位置（相对屏幕中心偏移）。
// 正数：x 向右 / y 向下。
constexpr int kHandsOffsetX = -14;
constexpr int kHandsOffsetY = -6;

// ---------------------------------------------------------------------------
// 【五】弧排文字 "DAILY MISSION"
//      由 kMissionCharCount 个单字符 label 等角度摆放，不是一个整体 label。
// ---------------------------------------------------------------------------
// ★ 性能说明：每个单字符 label 都是引擎的独立绘制对象，每块刷新时都要按
//   其包围盒 ∩ 块 逐像素混合。因此 kMissionCharW/H 不要给太大余量 ——
//   13 个字符的包围盒面积会直接叠加到每帧开销里。
//   内置字体 20px 的单个大写字母约 11x15，给到 16x20 已有余量。
// 注意：这组文字有【自己的圆心】，与进度环的圆心是两套独立设置。
// 想让文字贴合进度环，请把这里的圆心设成与【六】的圆环圆心一致，
// 并让 kMissionRadius 接近 kArcRadius（建议略小 2~6 像素，让字压在环内侧）。
//   kMissionCenterX/Y 用的是【屏幕坐标】(0~360，原点左上角)，
//   而 kArcCenterX/Y 用的是【画布坐标】；因为画布与屏幕等大且居中，
//   两者数值含义相同，可直接对齐。
constexpr int kMissionRadius  = 130;  // 字符分布半径，越小越往圆心收
constexpr int kMissionCenterX = 180;  // ★ 文字弧圆心 X（屏幕坐标，180=水平正中）
constexpr int kMissionCenterY = 180;  // ★ 文字弧圆心 Y（屏幕坐标，180=垂直正中）
constexpr int kMissionSpanDeg = 110;  // 弧总张角（度），越大摊得越开
constexpr int kMissionMidDeg  = 100;  // 弧中心朝向（度，0=12点，顺时针）
constexpr int kMissionCharW   = 16;   // 单个字符 label 宽（原 28，收紧以省混合开销）
constexpr int kMissionCharH   = 20;   // 单个字符 label 高（原 32）

// ---------------------------------------------------------------------------
// 【六】进度环（就是屏幕外圈那个圆环）
// ---------------------------------------------------------------------------
// 画布是 360x360 的正方形，对象以 GFX_ALIGN_CENTER 对齐屏幕中心，
// 即【画布中心 = 屏幕中心】。圆环画在画布内的哪个位置，由下面
// kArcCenterX / kArcCenterY 决定（画布坐标，原点在画布左上角）：
//   画布中心 = (180, 180) = 屏幕中心。
//   想让圆环整体左移，减小 kArcCenterX；上移则减小 kArcCenterY。
// 换算式： 相对屏幕中心的偏移 = (kArcCenterX - 180, kArcCenterY - 180)
constexpr int kArcSize   = 360;   // 弧画布边长（固定 360，一般不用改）
constexpr int kArcCenterX = 180;  // ★ 圆环圆心 X（画布坐标，180 = 屏幕水平正中）
constexpr int kArcCenterY = 180;  // ★ 圆环圆心 Y（画布坐标，180 = 屏幕垂直正中）
constexpr int kArcRadius = 150;   // ★ 圆环半径：想放大/缩小外圈就改这里
constexpr int kArcWidth  = 7;     // ★ 圆环线宽（粗细）

// 整块画布相对屏幕中心的额外平移（一般保持 0，位置请优先用上面的 kArcCenterX/Y）。
// 存在的意义：画布只有 360x360，若把圆环画在很靠边的位置会超出画布被裁，
// 此时可用这两个值把整块画布挪一挪。
constexpr int kArcObjOffsetX = 0;
constexpr int kArcObjOffsetY = 0;

// 圆环起止角度（度，0 = 12 点方向，顺时针为正）。
// 底环一般画整圈(0~360)；进度弧从 kArcProgStartDeg 开始扫过 pct% 的圆周。
// 想做成"缺口圆环"（例如底部留口，类似参考图），把底环改成 0~300 之类即可。
constexpr float kArcTrackStartDeg = 0.0f;    // 底环起始角
constexpr float kArcTrackEndDeg   = 360.0f;  // 底环结束角（360=整圈）
constexpr float kArcProgStartDeg  = 0.0f;    // 进度弧起始角（即 0% 的位置）

// 圆环配色
#define CLOCKVIEW_COLOR_ARC_TRACK     rgb565(90, 100, 110)   // 底环（暗灰，整圈）
#define CLOCKVIEW_COLOR_ARC_PROGRESS  rgb565(90, 200, 255)   // 进度弧（青蓝）

// ---------------------------------------------------------------------------
// 【七】模拟表盘几何（图层内部坐标，原点 = 图层中心）
// ---------------------------------------------------------------------------
// 图层边长 kHandsLayerSize 定义在 clock_view.h（因为 EmoteDisplay 也要用）。
//
// ★★ 重要硬约束（改这两个值时必须同时满足）★★
//   1) kDialRadius + 6  <=  kHandsLayerSize / 2
//      否则表盘最外圈的刻度会被图层边界裁掉。
//      当前：64 + 6 = 70  <=  140/2 = 70  ✓ 刚好贴合
//      想放大表盘，请【同时】把 clock_view.h 里的 kHandsLayerSize 改大：
//        kDialRadius=70  -> kHandsLayerSize=150
//        kDialRadius=80  -> kHandsLayerSize=170
//   2) 三根指针长度建议都 < kDialRadius，否则指针会戳出表盘外圈。
//
// 三根指针的相对比例参考图约为  时针:分针:秒针 = 0.6 : 0.85 : 1.0
// 想整体缩放指针，保持这个比例改即可。
// ---------------------------------------------------------------------------
constexpr int kHandsSize = clock_view::kHandsLayerSize;

constexpr int kDialRadius    = 64;   // 表盘外圈半径（刻度所在圆）
constexpr int kDialRingWidth = 2;    // 表盘外圈线宽
constexpr int kHourTickLen   = 9;    // 小时刻度长度（从外圈往内）
constexpr int kHourTickW     = 2;    // 小时刻度粗细
constexpr int kMinTickLen    = 5;    // 分钟刻度长度
constexpr int kMinTickW      = 1;    // 分钟刻度粗细

constexpr int kHourHandLen = 38;     // 时针长度（约 0.60 * kDialRadius）
constexpr int kHourHandW   = 7;      // 时针粗细
constexpr int kMinHandLen  = 54;     // 分针长度（约 0.85 * kDialRadius）
constexpr int kMinHandW    = 5;      // 分针粗细
constexpr int kSecHandLen  = 60;     // 秒针长度（约 0.94 * kDialRadius，勿 >= kDialRadius）
constexpr int kSecHandW    = 2;      // 秒针粗细
constexpr int kSecTailLen  = 10;     // 秒针反向尾长（0=不画尾）
constexpr int kHubRadius   = 4;      // 中心圆点半径

// 表盘配色
#define CLOCKVIEW_COLOR_DIAL_RING  rgb565(70, 78, 88)      // 表盘外圈
#define CLOCKVIEW_COLOR_TICK_HOUR  rgb565(200, 210, 220)   // 小时刻度（亮）
#define CLOCKVIEW_COLOR_TICK_MIN   rgb565(110, 120, 132)   // 分钟刻度（暗）
#define CLOCKVIEW_COLOR_HOUR_HAND  rgb565(235, 240, 245)   // 时针（白）
#define CLOCKVIEW_COLOR_MIN_HAND   rgb565(90, 200, 255)    // 分针（青）
#define CLOCKVIEW_COLOR_SEC_HAND   rgb565(255, 70, 70)     // 秒针（红）
#define CLOCKVIEW_COLOR_HUB        rgb565(240, 60, 60)     // 中心圆点（红）

// ---------------------------------------------------------------------------
// 【八】各文字颜色（十六进制 RRGGBB，直接照抄取色器里的值即可）
// ---------------------------------------------------------------------------
#define CLOCKVIEW_COLOR_WHITE      GFX_COLOR_HEX(0xFFFFFF)  // 通用白
#define CLOCKVIEW_COLOR_TIME_HH    GFX_COLOR_HEX(0x5AC8FF)  // 数字页小时（青）
#define CLOCKVIEW_COLOR_TIME_COLON GFX_COLOR_HEX(0x5AC8FF)  // 数字页冒号（青）
#define CLOCKVIEW_COLOR_TIME_MM    GFX_COLOR_HEX(0xFF4646)  // 数字页分钟（红）
#define CLOCKVIEW_COLOR_STEPS_PCT  GFX_COLOR_HEX(0x50E66E)  // 完成度 & 弧排文字（绿）

// ===========================================================================
// ★★★  可  调  参  数  区  结  束  ★★★
// ===========================================================================


// ---------------------------------------------------------------------------
// 模块状态
// ---------------------------------------------------------------------------
uint8_t* g_arc_buf = nullptr;        // 进度弧画布（RGB565A8，kArcSize^2*3）
gfx_image_dsc_t g_arc_dsc = {0};
bool g_arc_dsc_ready = false;
int  g_arc_last_pct = -1;            // 进度弧去重：上次绘制的百分比

uint8_t* g_hands_buf = nullptr;      // 指针图层画布（RGB565A8，kHandsSize^2*3）
gfx_image_dsc_t g_hands_dsc = {0};
bool g_hands_dsc_ready = false;
int  g_hands_last_sec = -1;          // 指针去重：上次绘制的"秒"

clock_view::ClockFace g_face = clock_view::ClockFace::DIGITAL;

// 文本去重缓存（独立缓存，避免字段之间互相覆盖）
char g_cache_hh[4] = {0};
char g_cache_mm[4] = {0};
char g_cache_colon[4] = {0};
char g_cache_batt[8] = {0};
char g_cache_steps[12] = {0};
char g_cache_steps_pct[8] = {0};
char g_cache_date[48] = {0};
char g_cache_small_time[8] = {0};
char g_cache_date_right[48] = {0};

// 字段级去重标记（比字符串比较更轻，且能表达"尚未刷新过"）
int g_seen_hh = -1;
int g_seen_mm = -1;
int g_seen_batt = -1;
int g_seen_steps = -1;
int g_seen_steps_pct = -1;
int g_seen_date_key = -1;    // 以 tm_yday 作为日期是否变化的键
int g_seen_small_key = -1;   // 模拟页小号时间：以 hh*60+mm 为键

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// RGB565A8 layout: [RGB565 plane: w*h*2 bytes][alpha plane: w*h bytes]
inline void set_pixel(uint8_t* buf, int w, int h, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    uint16_t* px = reinterpret_cast<uint16_t*>(
        buf + static_cast<size_t>(y) * w * 2 + static_cast<size_t>(x) * 2);
    *px = color;
    buf[static_cast<size_t>(w) * h * 2 + static_cast<size_t>(y) * w + x] = 0xFF;
}

void clear_buffer(uint8_t* buf, int w, int h)
{
    memset(buf, 0, static_cast<size_t>(w) * h * 3);
}

// 通用图像描述符初始化（RGB565A8，stride = w*3，与官方生成脚本一致）
void init_image_dsc(gfx_image_dsc_t& dsc, uint8_t* buf, int size, size_t bytes)
{
    memset(&dsc, 0, sizeof(gfx_image_dsc_t));
    dsc.header.magic = 0x19;
    dsc.header.cf = 0x0A;  // GFX_COLOR_FORMAT_RGB565A8
    dsc.header.w = static_cast<uint16_t>(size);
    dsc.header.h = static_cast<uint16_t>(size);
    dsc.header.stride = static_cast<uint16_t>(size * 3);
    dsc.data = buf;
    dsc.data_size = bytes;
}

// ---------------------------------------------------------------------------
// 线段绘制（Bresenham），用于时钟指针
// ---------------------------------------------------------------------------
void draw_line(uint8_t* buf, int w, int h, int x0, int y0, int x1, int y1,
               int thickness, uint16_t color)
{
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const int half = thickness / 2;

    while (true) {
        // 以 thickness 为边长画一个方形笔头，保证指针有可见宽度
        for (int oy = -half; oy <= half; ++oy) {
            for (int ox = -half; ox <= half; ++ox) {
                set_pixel(buf, w, h, x0 + ox, y0 + oy, color);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void fill_circle(uint8_t* buf, int w, int h, int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            if (x * x + y * y <= r * r) {
                set_pixel(buf, w, h, cx + x, cy + y, color);
            }
        }
    }
}

void draw_circle_outline(uint8_t* buf, int w, int h, int cx, int cy, int r,
                         int thickness, uint16_t color)
{
    const int r_in = r - thickness;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const int d2 = x * x + y * y;
            if (d2 <= r * r && d2 >= r_in * r_in) {
                set_pixel(buf, w, h, cx + x, cy + y, color);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 进度弧绘制
// 角度约定：12 点为 0°，顺时针增长；屏幕 y 向下，故 y = cy - r*cos(a)。
// ---------------------------------------------------------------------------
void draw_arc_ring(uint8_t* buf, int w, int h, int cx, int cy, int r, int thickness,
                   float a_start, float a_end, uint16_t color)
{
    if (a_end <= a_start || r <= 0 || thickness <= 0) {
        return;
    }
    const int steps = static_cast<int>((a_end - a_start) * static_cast<float>(r)) + 1;
    const int r_in = r - thickness / 2;
    const int r_out = r + thickness / 2;
    for (int i = 0; i <= steps; ++i) {
        const float a = a_start + (a_end - a_start) * static_cast<float>(i) / static_cast<float>(steps);
        const float sx = sinf(a);
        const float sy = cosf(a);
        for (int rr = r_in; rr <= r_out; ++rr) {
            set_pixel(buf, w, h,
                      cx + static_cast<int>(static_cast<float>(rr) * sx),
                      cy - static_cast<int>(static_cast<float>(rr) * sy),
                      color);
        }
    }
}

// ---------------------------------------------------------------------------
// 进度弧缓冲
// ---------------------------------------------------------------------------
bool ensure_arc_buffer()
{
    if (g_arc_buf != nullptr && g_arc_dsc_ready) {
        return true;
    }

    const size_t size = static_cast<size_t>(kArcSize) * kArcSize * 3;
    if (g_arc_buf == nullptr) {
        g_arc_buf = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
        if (g_arc_buf == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate arc buffer (%u bytes) in PSRAM", (unsigned)size);
            return false;
        }
    }
    init_image_dsc(g_arc_dsc, g_arc_buf, kArcSize, size);
    g_arc_dsc_ready = true;
    return true;
}

// 重绘进度弧。pct: 0-100。
void render_arc(gfx_obj_t* arc, int pct)
{
    if (!ensure_arc_buffer()) {
        return;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const int w = kArcSize;
    const int h = kArcSize;
    const int cx = kArcCenterX;   // 圆环圆心（画布坐标，可调）
    const int cy = kArcCenterY;
    const float kPi = 3.14159265f;
    const float kTwoPi = 2.0f * kPi;
    const float deg2rad = kPi / 180.0f;

    clear_buffer(g_arc_buf, w, h);

    // 底环（默认整圈暗色）
    draw_arc_ring(g_arc_buf, w, h, cx, cy, kArcRadius, kArcWidth,
                  kArcTrackStartDeg * deg2rad, kArcTrackEndDeg * deg2rad,
                  CLOCKVIEW_COLOR_ARC_TRACK);
    // 进度弧：从 kArcProgStartDeg 顺时针扫过 pct% 的圆周
    const float sweep = kTwoPi * static_cast<float>(pct) / 100.0f;
    if (sweep > 0.0f) {
        const float a0 = kArcProgStartDeg * deg2rad;
        draw_arc_ring(g_arc_buf, w, h, cx, cy, kArcRadius, kArcWidth,
                      a0, a0 + sweep, CLOCKVIEW_COLOR_ARC_PROGRESS);
    }

    g_arc_last_pct = pct;
    if (arc != nullptr) {
        gfx_img_set_src(arc, &g_arc_dsc);
    }
}

// ---------------------------------------------------------------------------
// 模拟时钟指针图层
// ---------------------------------------------------------------------------
bool ensure_hands_buffer()
{
    if (g_hands_buf != nullptr && g_hands_dsc_ready) {
        return true;
    }

    const size_t size = static_cast<size_t>(kHandsSize) * kHandsSize * 3;
    if (g_hands_buf == nullptr) {
        g_hands_buf = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
        if (g_hands_buf == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate hands buffer (%u bytes) in PSRAM", (unsigned)size);
            return false;
        }
    }
    init_image_dsc(g_hands_dsc, g_hands_buf, kHandsSize, size);
    g_hands_dsc_ready = true;
    return true;
}

// 绘制表盘：运行时每帧只需画刻度与中心点。
// 刻度用整数步进的三角函数，仅在 140x140 图层内做 12+60 次，
// 相比全屏表盘开销可忽略；且指针图层的 flush 区域也只有 140x140。
void draw_dial(uint8_t* buf, int cx, int cy)
{
    const float kPi = 3.14159265f;

    // 外圈
    draw_circle_outline(buf, kHandsSize, kHandsSize, cx, cy, kDialRadius,
                        kDialRingWidth, CLOCKVIEW_COLOR_DIAL_RING);

    // 12 个小时刻度（较长、较亮）
    for (int i = 0; i < 12; ++i) {
        const float a = static_cast<float>(i) * (2.0f * kPi / 12.0f);
        const float sx = sinf(a);
        const float sy = cosf(a);
        const int x0 = cx + static_cast<int>(static_cast<float>(kDialRadius - kHourTickLen) * sx);
        const int y0 = cy - static_cast<int>(static_cast<float>(kDialRadius - kHourTickLen) * sy);
        const int x1 = cx + static_cast<int>(static_cast<float>(kDialRadius - 1) * sx);
        const int y1 = cy - static_cast<int>(static_cast<float>(kDialRadius - 1) * sy);
        draw_line(buf, kHandsSize, kHandsSize, x0, y0, x1, y1, kHourTickW, CLOCKVIEW_COLOR_TICK_HOUR);
    }

    // 60 个分钟刻度（只画非整点位置，较短较暗）
    for (int i = 0; i < 60; ++i) {
        if (i % 5 == 0) {
            continue;  // 整点位置已由上面绘制
        }
        const float a = static_cast<float>(i) * (2.0f * kPi / 60.0f);
        const float sx = sinf(a);
        const float sy = cosf(a);
        const int x0 = cx + static_cast<int>(static_cast<float>(kDialRadius - kMinTickLen) * sx);
        const int y0 = cy - static_cast<int>(static_cast<float>(kDialRadius - kMinTickLen) * sy);
        const int x1 = cx + static_cast<int>(static_cast<float>(kDialRadius - 1) * sx);
        const int y1 = cy - static_cast<int>(static_cast<float>(kDialRadius - 1) * sy);
        draw_line(buf, kHandsSize, kHandsSize, x0, y0, x1, y1, kMinTickW, CLOCKVIEW_COLOR_TICK_MIN);
    }
}

// 依据当前时间重绘指针图层。仅当"秒"变化时由 Update 调用。
void render_hands(gfx_obj_t* hands, int hour, int minute, int second)
{
    if (!ensure_hands_buffer()) {
        return;
    }

    const int cx = kHandsSize / 2;
    const int cy = kHandsSize / 2;
    const float kPi = 3.14159265f;

    clear_buffer(g_hands_buf, kHandsSize, kHandsSize);
    // 表盘（静态几何，每帧重画，代价局限在 140x140 内）
    draw_dial(g_hands_buf, cx, cy);

    // 指针角度：12 点为 0°，顺时针。y 向下 => y = cy - r*cos(a)
    const float hour_a = (static_cast<float>(hour % 12) + static_cast<float>(minute) / 60.0f)
                         * (2.0f * kPi / 12.0f);
    const float min_a = (static_cast<float>(minute) + static_cast<float>(second) / 60.0f)
                        * (2.0f * kPi / 60.0f);
    const float sec_a = static_cast<float>(second) * (2.0f * kPi / 60.0f);

    auto hand_tip = [cx, cy](float a, int len, int& tx, int& ty) {
        tx = cx + static_cast<int>(static_cast<float>(len) * sinf(a));
        ty = cy - static_cast<int>(static_cast<float>(len) * cosf(a));
    };

    int tx = 0;
    int ty = 0;

    // 时针（白色粗针）
    hand_tip(hour_a, kHourHandLen, tx, ty);
    draw_line(g_hands_buf, kHandsSize, kHandsSize, cx, cy, tx, ty, kHourHandW, CLOCKVIEW_COLOR_HOUR_HAND);

    // 分针（青色）
    hand_tip(min_a, kMinHandLen, tx, ty);
    draw_line(g_hands_buf, kHandsSize, kHandsSize, cx, cy, tx, ty, kMinHandW, CLOCKVIEW_COLOR_MIN_HAND);

    // 秒针（红色细针，可带反向尾；kSecTailLen=0 时不画尾）
    hand_tip(sec_a, kSecHandLen, tx, ty);
    const int tail_x = cx - static_cast<int>(static_cast<float>(kSecTailLen) * sinf(sec_a));
    const int tail_y = cy + static_cast<int>(static_cast<float>(kSecTailLen) * cosf(sec_a));
    draw_line(g_hands_buf, kHandsSize, kHandsSize, tail_x, tail_y, tx, ty, kSecHandW, CLOCKVIEW_COLOR_SEC_HAND);

    // 中心圆点
    fill_circle(g_hands_buf, kHandsSize, kHandsSize, cx, cy, kHubRadius, CLOCKVIEW_COLOR_HUB);

    g_hands_last_sec = second;
    if (hands != nullptr) {
        gfx_img_set_src(hands, &g_hands_dsc);
    }
}

// ---------------------------------------------------------------------------
// 弧排字符定位：把 "DAILY MISSION" 的每个字符沿弧线等角度摆放。
// 只在 Init 阶段调用一次 —— gfx_obj_align 会置 is_dirty，
// 每帧调用会产生冗余脏标记，而字符位置是固定的。
// ---------------------------------------------------------------------------
void layout_mission_chars(gfx_obj_t** mission)
{
    if (mission == nullptr) {
        return;
    }
    const float kPi = 3.14159265f;
    const float span = static_cast<float>(kMissionSpanDeg) * kPi / 180.0f;
    const float mid = static_cast<float>(kMissionMidDeg) * kPi / 180.0f;
    const float step = span / static_cast<float>(clock_view::kMissionCharCount - 1);
    for (int i = 0; i < clock_view::kMissionCharCount; ++i) {
        gfx_obj_t* ch = mission[i];
        if (ch == nullptr) {
            continue;
        }
        const float a = mid - span / 2.0f + step * static_cast<float>(i);
        const int x = kMissionCenterX + static_cast<int>(static_cast<float>(kMissionRadius) * cosf(a));
        const int y = kMissionCenterY + static_cast<int>(static_cast<float>(kMissionRadius) * sinf(a));
        gfx_obj_align(ch, GFX_ALIGN_CENTER, x - kScreenW / 2, y - kScreenH / 2);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace clock_view {

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
          gfx_obj_t*& out_hands)
{
    auto* const handle = static_cast<gfx_handle_t>(engine);

    // ---- 顶部电池百分比（两页共用）----
    out_battery_pct = gfx_label_create(handle);
    gfx_obj_align(out_battery_pct, GFX_ALIGN_TOP_MID, 0, kBatteryY);
    gfx_obj_set_size(out_battery_pct, kBatteryW, kBatteryH);
    gfx_label_set_text(out_battery_pct, "--");
    gfx_label_set_color(out_battery_pct, CLOCKVIEW_COLOR_WHITE);
    gfx_label_set_text_align(out_battery_pct, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_set_visible(out_battery_pct, false);

    // ---- 数字页：日期行 ----
    out_date = gfx_label_create(handle);
    gfx_obj_align(out_date, GFX_ALIGN_TOP_MID, 0, kDateY);
    gfx_obj_set_size(out_date, kDateW, kDateH);
    gfx_label_set_text(out_date, "FRI 18 JUL 2025");
    gfx_label_set_color(out_date, CLOCKVIEW_COLOR_WHITE);
    gfx_label_set_text_align(out_date, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_set_visible(out_date, false);

    // ---- 数字页：中央时钟：时 / 冒号 / 分 三个独立 label ----
    // 受 flash 容量限制只能复用 BUILTIN_TEXT_FONT（echoear 为 20px），
    // 所以这里只调位置/间距，实际字号由字体决定。
    out_time_hh = gfx_label_create(handle);
    gfx_obj_align(out_time_hh, GFX_ALIGN_CENTER, kTimeHhX, kTimeOffsetY);
    gfx_obj_set_size(out_time_hh, kTimeHhW, kTimeHhH);
    gfx_label_set_text(out_time_hh, "19");
    gfx_label_set_color(out_time_hh, CLOCKVIEW_COLOR_TIME_HH);
    gfx_label_set_text_align(out_time_hh, GFX_TEXT_ALIGN_RIGHT);
    gfx_obj_set_visible(out_time_hh, false);

    out_time_colon = gfx_label_create(handle);
    gfx_obj_align(out_time_colon, GFX_ALIGN_CENTER, kTimeColonX, kTimeOffsetY);
    gfx_obj_set_size(out_time_colon, kTimeColonW, kTimeColonH);
    gfx_label_set_text(out_time_colon, ":");
    gfx_label_set_color(out_time_colon, CLOCKVIEW_COLOR_TIME_COLON);
    gfx_label_set_text_align(out_time_colon, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_set_visible(out_time_colon, false);

    out_time_mm = gfx_label_create(handle);
    gfx_obj_align(out_time_mm, GFX_ALIGN_CENTER, kTimeMmX, kTimeOffsetY);
    gfx_obj_set_size(out_time_mm, kTimeMmW, kTimeMmH);
    gfx_label_set_text(out_time_mm, "42");
    gfx_label_set_color(out_time_mm, CLOCKVIEW_COLOR_TIME_MM);
    gfx_label_set_text_align(out_time_mm, GFX_TEXT_ALIGN_LEFT);
    gfx_obj_set_visible(out_time_mm, false);

    // ---- 步数完成度百分比（两页共用）----
    out_steps_pct = gfx_label_create(handle);
    gfx_obj_align(out_steps_pct, GFX_ALIGN_CENTER, 0, kStepsPctY - kScreenH / 2);
    gfx_obj_set_size(out_steps_pct, kStepsPctW, kStepsPctH);
    gfx_label_set_text(out_steps_pct, "87%");
    gfx_label_set_color(out_steps_pct, CLOCKVIEW_COLOR_STEPS_PCT);
    gfx_label_set_text_align(out_steps_pct, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_set_visible(out_steps_pct, false);

    // ---- 步数（两页共用）----
    out_steps = gfx_label_create(handle);
    gfx_obj_align(out_steps, GFX_ALIGN_CENTER, 0, kStepsY - kScreenH / 2);
    gfx_obj_set_size(out_steps, kStepsW, kStepsH);
    gfx_label_set_text(out_steps, "1526");
    gfx_label_set_color(out_steps, CLOCKVIEW_COLOR_WHITE);
    gfx_label_set_text_align(out_steps, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_set_visible(out_steps, false);

    // ---- 弧形 "DAILY MISSION"：等角度分布的单字符 label（两页共用）----
    static const char kMissionText[] = "DAILY MISSION";
    const int mission_count = static_cast<int>(sizeof(kMissionText) - 1);  // 13
    for (int i = 0; i < mission_count && i < kMissionCharCount; ++i) {
        out_mission[i] = gfx_label_create(handle);
        char ch[2] = {kMissionText[i], '\0'};
        gfx_label_set_text(out_mission[i], ch);
        gfx_label_set_color(out_mission[i], CLOCKVIEW_COLOR_STEPS_PCT);
        gfx_label_set_text_align(out_mission[i], GFX_TEXT_ALIGN_CENTER);
        gfx_obj_set_size(out_mission[i], kMissionCharW, kMissionCharH);
        gfx_obj_set_visible(out_mission[i], false);
    }
    layout_mission_chars(out_mission);

    // ---- 进度弧（两页共用，仅百分比变化时重绘）----
    // 画布 360x360 与屏幕等大，以屏幕中心对齐，因此画布坐标即屏幕坐标，
    // 圆环的实际位置由 kArcCenterX/Y 决定（见【六】）。
    out_arc = gfx_img_create(handle);
    gfx_obj_align(out_arc, GFX_ALIGN_CENTER, kArcObjOffsetX, kArcObjOffsetY);
    gfx_obj_set_visible(out_arc, false);

    // ---- 模拟页：顶部小号时间 ----
    out_time_small = gfx_label_create(handle);
    gfx_obj_align(out_time_small, GFX_ALIGN_TOP_MID, 0, kSmallTimeY);
    gfx_obj_set_size(out_time_small, kSmallTimeW, kSmallTimeH);
    gfx_label_set_text(out_time_small, "19:42");
    gfx_label_set_color(out_time_small, CLOCKVIEW_COLOR_WHITE);
    gfx_label_set_text_align(out_time_small, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_set_visible(out_time_small, false);

    // ---- 模拟页：右侧日期 ----
    out_date_right = gfx_label_create(handle);
    gfx_obj_align(out_date_right, GFX_ALIGN_CENTER, kDateRightX, kDateRightY);
    gfx_obj_set_size(out_date_right, kDateRightW, kDateRightH);
    gfx_label_set_text(out_date_right, "FRI\n18 JUL\n2025");
    gfx_label_set_color(out_date_right, CLOCKVIEW_COLOR_WHITE);
    gfx_label_set_text_align(out_date_right, GFX_TEXT_ALIGN_CENTER);
    gfx_obj_set_visible(out_date_right, false);

    // ---- 模拟页：指针图层（小尺寸自绘图像，仅秒变化时重绘）----
    out_hands = gfx_img_create(handle);
    gfx_obj_align(out_hands, GFX_ALIGN_CENTER, kHandsOffsetX, kHandsOffsetY);
    gfx_obj_set_size(out_hands, kHandsSize, kHandsSize);
    gfx_obj_set_visible(out_hands, false);

    // 重置去重状态，确保首次 Update 全量刷新
    g_cache_hh[0] = '\0';
    g_cache_mm[0] = '\0';
    g_cache_colon[0] = '\0';
    g_cache_batt[0] = '\0';
    g_cache_steps[0] = '\0';
    g_cache_steps_pct[0] = '\0';
    g_cache_date[0] = '\0';
    g_cache_small_time[0] = '\0';
    g_cache_date_right[0] = '\0';
    g_seen_hh = -1;
    g_seen_mm = -1;
    g_seen_batt = -1;
    g_seen_steps = -1;
    g_seen_steps_pct = -1;
    g_seen_date_key = -1;
    g_seen_small_key = -1;
    g_arc_last_pct = -1;
    g_hands_last_sec = -1;
    g_face = ClockFace::DIGITAL;

    ESP_LOGI(TAG, "Clock view initialized (digital+analog, %d mission chars, hands layer %dx%d)",
             mission_count, kHandsSize, kHandsSize);
}

void SetFonts(gfx_obj_t* label_battery_pct, gfx_obj_t* label_date,
              gfx_obj_t* label_time_hh, gfx_obj_t* label_time_mm, gfx_obj_t* label_time_colon,
              gfx_obj_t* label_steps_pct, gfx_obj_t* label_steps, gfx_obj_t** mission,
              gfx_obj_t* label_time_small, gfx_obj_t* label_date_right,
              void* big_font, void* mid_font, void* small_font)
{
    if (label_battery_pct != nullptr && mid_font != nullptr) {
        gfx_label_set_font(label_battery_pct, static_cast<gfx_font_t>(mid_font));
    }
    if (label_date != nullptr && mid_font != nullptr) {
        gfx_label_set_font(label_date, static_cast<gfx_font_t>(mid_font));
    }
    if (label_time_hh != nullptr && big_font != nullptr) {
        gfx_label_set_font(label_time_hh, static_cast<gfx_font_t>(big_font));
    }
    if (label_time_mm != nullptr && big_font != nullptr) {
        gfx_label_set_font(label_time_mm, static_cast<gfx_font_t>(big_font));
    }
    if (label_time_colon != nullptr && big_font != nullptr) {
        gfx_label_set_font(label_time_colon, static_cast<gfx_font_t>(big_font));
    }
    if (label_steps_pct != nullptr && mid_font != nullptr) {
        gfx_label_set_font(label_steps_pct, static_cast<gfx_font_t>(mid_font));
    }
    if (label_steps != nullptr && mid_font != nullptr) {
        gfx_label_set_font(label_steps, static_cast<gfx_font_t>(mid_font));
    }
    if (label_time_small != nullptr && mid_font != nullptr) {
        gfx_label_set_font(label_time_small, static_cast<gfx_font_t>(mid_font));
    }
    if (label_date_right != nullptr && mid_font != nullptr) {
        gfx_label_set_font(label_date_right, static_cast<gfx_font_t>(mid_font));
    }
    // 弧排字符的字体由调用方对数组内每个元素统一设置（见 SetupUI），
    // 这里只兜底处理首个元素，避免漏设。
    if (mission != nullptr && mission[0] != nullptr && small_font != nullptr) {
        gfx_label_set_font(mission[0], static_cast<gfx_font_t>(small_font));
    }
}

ClockFace ToggleFace()
{
    g_face = (g_face == ClockFace::DIGITAL) ? ClockFace::ANALOG : ClockFace::DIGITAL;
    // 切换后强制下次刷新一次该界面的动态内容
    g_hands_last_sec = -1;
    g_seen_hh = -1;
    g_seen_mm = -1;
    g_seen_small_key = -1;
    return g_face;
}

ClockFace CurrentFace()
{
    return g_face;
}

void ApplyFaceVisibility(gfx_obj_t* label_battery_pct,
                         gfx_obj_t* label_date,
                         gfx_obj_t* label_time_hh, gfx_obj_t* label_time_mm, gfx_obj_t* label_time_colon,
                         gfx_obj_t* label_steps_pct, gfx_obj_t* label_steps, gfx_obj_t** mission,
                         gfx_obj_t* arc,
                         gfx_obj_t* label_time_small, gfx_obj_t* label_date_right, gfx_obj_t* hands)
{
    const bool digital = (g_face == ClockFace::DIGITAL);

    auto show = [](gfx_obj_t* obj, bool visible) {
        if (obj != nullptr) {
            gfx_obj_set_visible(obj, visible);
        }
    };

    // 两页共用元素
    show(label_battery_pct, true);
    show(label_steps_pct, true);
    show(label_steps, true);
    show(arc, true);
    if (mission != nullptr) {
        for (int i = 0; i < kMissionCharCount; ++i) {
            show(mission[i], true);
        }
    }

    // 数字页专属
    show(label_date, digital);
    show(label_time_hh, digital);
    show(label_time_mm, digital);
    show(label_time_colon, digital);

    // 模拟页专属
    show(label_time_small, !digital);
    show(label_date_right, !digital);
    show(hands, !digital);
}

void Update(gfx_obj_t* label_battery_pct, gfx_obj_t* label_date,
            gfx_obj_t* label_time_hh, gfx_obj_t* label_time_mm, gfx_obj_t* label_time_colon,
            gfx_obj_t* label_steps_pct, gfx_obj_t* label_steps, gfx_obj_t** mission, gfx_obj_t* arc,
            gfx_obj_t* label_time_small, gfx_obj_t* label_date_right, gfx_obj_t* hands,
            int battery_value, int steps_value, int steps_goal)
{
    // 弧排字符的内容与位置均固定，Update 不需要触碰 mission。
    (void)mission;

    time_t now;
    struct tm ti;
    time(&now);
    setenv("TZ", "GMT+0", 1);
    tzset();
    localtime_r(&now, &ti);

    char hh[4];
    char mm[4];
    // 8 字节对 "%d%%" 太小：%d 的 int 最大可输出 11 个字符（含负号），
    // 再拼 "%" 后 GCC 判定可能截断并报 -Werror=format-truncation。
    // 这里放大到 16，且下面只按 0..100 的合法范围格式化。
    char batt[16];
    char steps_str[12];
    char steps_pct_str[8];
    char date_str[48];
    static const char kWeekday[][4] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char kMonth[][4] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

    snprintf(hh, sizeof(hh), "%02d", ti.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", ti.tm_min);
    snprintf(date_str, sizeof(date_str), "%s %02d %s %04d",
             kWeekday[ti.tm_wday % 7], ti.tm_mday, kMonth[ti.tm_mon % 12], ti.tm_year + 1900);

    const bool digital = (g_face == ClockFace::DIGITAL);

    if (digital) {
        // ---- 数字页：时 / 冒号 / 分 三个 label 各自去重 ----
        if (label_time_hh != nullptr && ti.tm_hour != g_seen_hh) {
            g_seen_hh = ti.tm_hour;
            strncpy(g_cache_hh, hh, sizeof(g_cache_hh) - 1);
            g_cache_hh[sizeof(g_cache_hh) - 1] = '\0';
            gfx_label_set_text(label_time_hh, hh);
        }
        if (label_time_colon != nullptr && g_cache_colon[0] == '\0') {
            strncpy(g_cache_colon, ":", sizeof(g_cache_colon) - 1);
            gfx_label_set_text(label_time_colon, ":");
        }
        if (label_time_mm != nullptr && ti.tm_min != g_seen_mm) {
            g_seen_mm = ti.tm_min;
            strncpy(g_cache_mm, mm, sizeof(g_cache_mm) - 1);
            g_cache_mm[sizeof(g_cache_mm) - 1] = '\0';
            gfx_label_set_text(label_time_mm, mm);
        }

        // ---- 数字页：日期（以 tm_yday 作为变化键）----
        if (label_date != nullptr && ti.tm_yday != g_seen_date_key) {
            g_seen_date_key = ti.tm_yday;
            strncpy(g_cache_date, date_str, sizeof(g_cache_date) - 1);
            g_cache_date[sizeof(g_cache_date) - 1] = '\0';
            gfx_label_set_text(label_date, date_str);
        }
    } else {
        // ---- 模拟页：仅指针图层（每秒一次）+ 小号时间文本（分钟变化时）+ 日期 ----
        // 指针相比"秒"去重：同一秒内重复调用不会重复重绘。
        if (hands != nullptr && ti.tm_sec != g_hands_last_sec) {
            render_hands(hands, ti.tm_hour, ti.tm_min, ti.tm_sec);
        }

        const int small_key = ti.tm_hour * 60 + ti.tm_min;
        if (label_time_small != nullptr && small_key != g_seen_small_key) {
            g_seen_small_key = small_key;
            char small[8];
            snprintf(small, sizeof(small), "%02d:%02d", ti.tm_hour, ti.tm_min);
            strncpy(g_cache_small_time, small, sizeof(g_cache_small_time) - 1);
            g_cache_small_time[sizeof(g_cache_small_time) - 1] = '\0';
            gfx_label_set_text(label_time_small, small);
        }

        // 模拟页右侧日期：跨天时更新一次
        if (label_date_right != nullptr && ti.tm_yday != g_seen_date_key) {
            g_seen_date_key = ti.tm_yday;
            char right[48];
            snprintf(right, sizeof(right), "%s\n%02d %s\n%04d",
                     kWeekday[ti.tm_wday % 7], ti.tm_mday, kMonth[ti.tm_mon % 12], ti.tm_year + 1900);
            strncpy(g_cache_date_right, right, sizeof(g_cache_date_right) - 1);
            g_cache_date_right[sizeof(g_cache_date_right) - 1] = '\0';
            gfx_label_set_text(label_date_right, right);
        }
    }

    // ---- 电池（两页共用，各自去重）----
    // battery_value < 0 表示电量未知（BQ27220 未接/读取失败），显示 "--" 而不是 0%。
    if (label_battery_pct != nullptr && battery_value != g_seen_batt) {
        // 先夹到合法范围，保证下面的格式化输出长度可控
        int shown = battery_value;
        if (shown < -1) {
            shown = -1;
        }
        if (shown > 100) {
            shown = 100;
        }
        g_seen_batt = shown;
        if (shown < 0) {
            snprintf(batt, sizeof(batt), "--");
        } else {
            // 注意："%.3d" 的精度是【最少】位数，不是上限（对 int 仍可能 10 位），
            // 不能用来消除 -Wformat-truncation。回收成 uint8_t 后编译器才能算出上限。
            const uint8_t batt_u8 = static_cast<uint8_t>(shown);
            snprintf(batt, sizeof(batt), "%u%%", static_cast<unsigned>(batt_u8));
        }
        strncpy(g_cache_batt, batt, sizeof(g_cache_batt) - 1);
        g_cache_batt[sizeof(g_cache_batt) - 1] = '\0';
        gfx_label_set_text(label_battery_pct, batt);
    }

    // ---- 步数与完成度（两页共用，各自去重）----
    int pct = 0;
    if (steps_goal > 0) {
        pct = (steps_value * 100) / steps_goal;
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
    }
    if (label_steps_pct != nullptr && pct != g_seen_steps_pct) {
        g_seen_steps_pct = pct;
        // 同上：回收成 uint8_t，让编译器能证明输出上限（"%.3d" 做不到这点）
        const uint8_t pct_u8 = static_cast<uint8_t>(pct);
        snprintf(steps_pct_str, sizeof(steps_pct_str), "%u%%", static_cast<unsigned>(pct_u8));
        strncpy(g_cache_steps_pct, steps_pct_str, sizeof(g_cache_steps_pct) - 1);
        g_cache_steps_pct[sizeof(g_cache_steps_pct) - 1] = '\0';
        gfx_label_set_text(label_steps_pct, steps_pct_str);
    }
    if (label_steps != nullptr && steps_value != g_seen_steps) {
        // 步数理论上不该为负/超大；先夹到 0..999999
        int shown_steps = steps_value;
        if (shown_steps < 0) {
            shown_steps = 0;
        }
        if (shown_steps > 999999) {
            shown_steps = 999999;
        }
        g_seen_steps = shown_steps;
        // 分两段输出，避免依赖 int 的类型宽度推断：
        // 万位以上（0..99，最多 2 位）+ 低 4 位补零（最多 4 位），总长 <= 6 字节。
        const unsigned hi = static_cast<unsigned>(shown_steps / 10000u);
        const unsigned lo = static_cast<unsigned>(shown_steps % 10000u);
        if (hi > 0) {
            snprintf(steps_str, sizeof(steps_str), "%u%04u", hi, lo);
        } else {
            snprintf(steps_str, sizeof(steps_str), "%u", lo);
        }
        strncpy(g_cache_steps, steps_str, sizeof(g_cache_steps) - 1);
        g_cache_steps[sizeof(g_cache_steps) - 1] = '\0';
        gfx_label_set_text(label_steps, steps_str);
    }

    // ---- 进度弧（两页共用，仅在百分比变化时重绘 + set_src）----
    if (arc != nullptr && pct != g_arc_last_pct) {
        render_arc(arc, pct);
    }

    // 注意：弧排字符的位置在 Init 阶段已由 layout_mission_chars 一次性定位，
    // 此处不再调用 gfx_obj_align（它会置 is_dirty，每帧调用会产生冗余脏标记）。
}

void OnPageEnter()
{
    // 强制下次全部刷新
    g_cache_hh[0] = '\0';
    g_cache_mm[0] = '\0';
    g_cache_colon[0] = '\0';
    g_cache_batt[0] = '\0';
    g_cache_steps[0] = '\0';
    g_cache_steps_pct[0] = '\0';
    g_cache_date[0] = '\0';
    g_cache_small_time[0] = '\0';
    g_cache_date_right[0] = '\0';
    g_seen_hh = -1;
    g_seen_mm = -1;
    g_seen_batt = -1;
    g_seen_steps = -1;
    g_seen_steps_pct = -1;
    g_seen_date_key = -1;
    g_seen_small_key = -1;
    g_arc_last_pct = -1;
    g_hands_last_sec = -1;
}

}  // namespace clock_view
