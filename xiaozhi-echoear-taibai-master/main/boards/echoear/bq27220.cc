// ============================================================================
// bq27220.cc — TI BQ27220 单节锂电电量计驱动实现
// ----------------------------------------------------------------------------
// 设计取舍
// ----------------------------------------------------------------------------
// 1) 独立 i2c_master_dev，不继承 I2cDevice
//    I2cDevice::ReadRegs 内部用 ESP_ERROR_CHECK，一次 NAK 直接 panic。
//    电量计是"可能不存在"的外设（拔电池就掉电），必须能容错。
//
// 2) 失败不污染缓存
//    读失败时清 valid，且 GetLevel 返回 false，而不是返回 0%。
//    上层据此保持上一次显示值，避免电量"瞬间掉到 0% 又跳回来"。
//
// 3) 充放电判定：BatteryStatus 位域 + 电流符号双重确认
//    BatteryStatus(0x16)：
//      bit0 DSG  = 1 正在放电
//      bit1 FC   = 1 已充满
//      bit2 TDA  = 1 即将完全放电
//    不同量产配置对 FC/DSG 的行为略有差异，所以再用电流符号覆盖：
//      current >  +阈值 → 充电
//      current <  -阈值 → 放电
//      静置          → 以 DSG 位为准
//
// 4) 温度
//    BQ27220 的 Temperature 单位是 0.1 K，摄氏度 = 原始值 - 2731。
// ============================================================================
#include "bq27220.h"

#include <esp_log.h>

#define TAG "BQ27220"

namespace bq27220 {

namespace {

// ---- 标准命令（对照 TI 数据手册 SLUSCB7 的 Standard Commands 表）----
constexpr uint8_t kCmdTemperature       = 0x06;
constexpr uint8_t kCmdVoltage           = 0x08;
constexpr uint8_t kCmdAverageCurrent    = 0x0A;
constexpr uint8_t kCmdCurrent           = 0x0C;
constexpr uint8_t kCmdRemainingCapacity = 0x04;
constexpr uint8_t kCmdFullCapacity      = 0x10;
constexpr uint8_t kCmdTimeToEmpty       = 0x0E;
constexpr uint8_t kCmdBatteryStatus     = 0x16;
constexpr uint8_t kCmdStateOfCharge     = 0x2C;
constexpr uint8_t kCmdDesignCapacity    = 0x3C;

constexpr int kI2cTimeoutMs = 100;

// BatteryStatus 位定义
constexpr uint16_t kStatusDsg = 1u << 0;   // 放电中
constexpr uint16_t kStatusFc  = 1u << 1;   // 已充满
constexpr uint16_t kStatusTda = 1u << 2;   // 即将完全放电

// 电流绝对值超过该阈值才认定方向，滤掉静置噪声
constexpr int kCurrentDeadbandMa = 10;

}  // namespace

Bq27220::Bq27220(i2c_master_bus_handle_t i2c_bus, const uint8_t addr)
    : i2c_bus_(i2c_bus), addr_(addr)
{
    // 全局默认级别是 WARN；电量计属于要观察的关键外设，
    // 单独把本模块提到 INFO，让检测结果可见又不影响其它模块。
    esp_log_level_set(TAG, ESP_LOG_INFO);

    if (i2c_bus_ == nullptr) {
        ESP_LOGE(TAG, "i2c bus is null");
        return;
    }

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr_;
    cfg.scl_speed_hz = 100 * 1000;   // 电量计用 100k 更稳（同总线上的编码器是 400k）
    cfg.scl_wait_us = 0;
    cfg.flags.disable_ack_check = 0;

    const esp_err_t err = i2c_master_bus_add_device(i2c_bus_, &cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", addr_, esp_err_to_name(err));
        dev_ = nullptr;
        return;
    }
    dev_added_ = true;
}

Bq27220::~Bq27220()
{
    if (dev_added_ && dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
        dev_added_ = false;
    }
}

bool Bq27220::Probe()
{
    if (i2c_bus_ == nullptr) {
        present_ = false;
        return false;
    }
    const esp_err_t err = i2c_master_probe(i2c_bus_, addr_, kI2cTimeoutMs);
    present_ = (err == ESP_OK);
    if (!present_) {
        ESP_LOGW(TAG, "not found at 0x%02X (%s)", addr_, esp_err_to_name(err));
    }
    return present_;
}

bool Bq27220::ReadRaw(const uint8_t cmd, uint8_t* buf, const size_t len)
{
    if (dev_ == nullptr || buf == nullptr || len == 0) {
        return false;
    }
    const esp_err_t err =
        i2c_master_transmit_receive(dev_, &cmd, 1, buf, len, kI2cTimeoutMs);
    if (err != ESP_OK) {
        // 电池查询是周期性的，读失败只打 debug，避免刷屏
        ESP_LOGD(TAG, "read cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
        return false;
    }
    return true;
}

uint16_t Bq27220::ReadWord(const uint8_t cmd)
{
    uint8_t b[2] = {0, 0};
    if (!ReadRaw(cmd, b, 2)) {
        return 0;
    }
    return static_cast<uint16_t>(b[1] << 8 | b[0]);   // 小端
}

int16_t Bq27220::ReadSignedWord(const uint8_t cmd)
{
    return static_cast<int16_t>(ReadWord(cmd));
}

bool Bq27220::Read(BatteryStatus& out)
{
    if (!present_) {
        out = BatteryStatus{};
        return false;
    }

    // 先用 SOC 探活：这一次失败就认为芯片掉线（例如拔了电池）
    uint8_t soc_raw[2] = {0, 0};
    if (!ReadRaw(kCmdStateOfCharge, soc_raw, 2)) {
        present_ = false;              // 下次 GetLevel() 会重新 Probe
        out = BatteryStatus{};
        return false;
    }

    BatteryStatus s;
    s.valid = true;

    s.soc = static_cast<int>(static_cast<uint16_t>(soc_raw[1] << 8 | soc_raw[0]));
    if (s.soc > 100) {
        s.soc = 100;                   // 个别配置在满电后可能略超 100
    }
    if (s.soc < 0) {
        s.soc = 0;
    }

    s.voltage_mv      = static_cast<int>(ReadWord(kCmdVoltage));
    s.current_ma      = static_cast<int>(ReadSignedWord(kCmdCurrent));
    s.avg_current_ma  = static_cast<int>(ReadSignedWord(kCmdAverageCurrent));
    s.remaining_mah   = static_cast<int>(ReadWord(kCmdRemainingCapacity));
    s.full_mah        = static_cast<int>(ReadWord(kCmdFullCapacity));
    s.design_mah      = static_cast<int>(ReadWord(kCmdDesignCapacity));
    s.tte_min         = static_cast<int>(ReadWord(kCmdTimeToEmpty));
    s.status_flags    = ReadWord(kCmdBatteryStatus);

    // 温度：0.1 K → 0.1 ℃
    s.temperature_c10 = static_cast<int>(ReadWord(kCmdTemperature)) - 2731;

    const bool dsg_bit = (s.status_flags & kStatusDsg) != 0;
    const bool fc_bit  = (s.status_flags & kStatusFc) != 0;
    const bool tda_bit = (s.status_flags & kStatusTda) != 0;

    s.full = fc_bit;

    // 充放电判定（见文件头说明）
    if (s.current_ma > kCurrentDeadbandMa) {
        s.charging = true;
        s.discharging = false;
    } else if (s.current_ma < -kCurrentDeadbandMa) {
        s.charging = false;
        s.discharging = true;
    } else {
        // 静置：以位域为准，满充时不报放电
        s.charging = fc_bit;
        s.discharging = dsg_bit && !fc_bit;
    }
    // TDA（即将放空）也视为放电
    if (tda_bit && !s.charging) {
        s.discharging = true;
    }

    s.low_power = (s.soc < 20);

    last_ = s;
    out = s;
    return true;
}

bool Bq27220::GetLevel(int& level, bool& charging, bool& discharging)
{
    BatteryStatus s{};

    if (present_ && Read(s)) {
        level = s.soc;
        charging = s.charging;
        discharging = s.discharging;
        return true;
    }

    // 掉线后允许重新探测（Board 的轮询会周期走到这里）
    if (!present_ && Probe() && Read(s)) {
        level = s.soc;
        charging = s.charging;
        discharging = s.discharging;
        return true;
    }

    level = -1;
    charging = false;
    discharging = false;
    return false;
}

void Bq27220::Log() const
{
    if (!last_.valid) {
        ESP_LOGI(TAG, "battery: no data (present=%d)", present_ ? 1 : 0);
        return;
    }
    ESP_LOGI(TAG,
             "battery: %d%% %dmV %dmA %.1fC | rem/full/design=%d/%d/%d mAh | tte=%dmin | "
             "st=0x%04X chg=%d dsg=%d full=%d",
             last_.soc, last_.voltage_mv, last_.current_ma,
             static_cast<double>(last_.temperature_c10) / 10.0,
             last_.remaining_mah, last_.full_mah, last_.design_mah, last_.tte_min,
             last_.status_flags, last_.charging ? 1 : 0, last_.discharging ? 1 : 0,
             last_.full ? 1 : 0);
}

}  // namespace bq27220
