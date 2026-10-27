// ============================================================================
// bq27220.h — TI BQ27220 单节锂电电量计驱动（独立文件）
// ----------------------------------------------------------------------------
// 为什么单独一个文件：
//   原 EchoEar.cc 里的 Charge 类只把 0x08/0x0c 读出来就丢掉了，既没解析，
//   也没接到 Board::GetBatteryLevel()，所以界面只有图标、没有真实电量。
//   本文件把电量检测完整实现，并对外提供 Board 需要的统一形态。
//
// 硬件（EchoEar）：
//   I2C 地址 0x55，与音频编解码器/触摸共用 i2c_bus_（SDA=GPIO2 / SCL=GPIO1）
//   注意：BQ27220 的 VCC 由电池供电，拔掉电池后它会掉电，I2C 无响应。
//
// 标准命令（Standard Commands，2 字节小端），对照 TI 数据手册 SLUSCB7：
//   0x2C StateOfCharge       %       ← 主电量
//   0x08 Voltage            mV
//   0x0C Current            mA      有符号，负 = 放电
//   0x06 Temperature        0.1 K   需换算成 ℃（减 2731）
//   0x04 RemainingCapacity  mAh
//   0x10 FullChargeCapacity mAh
//   0x0A AverageCurrent     mA
//   0x0E TimeToEmpty        min
//   0x16 BatteryStatus      位域（DSG / FC / TDA / SYSDOWN ...）
//   0x3C DesignCapacity     mAh
// ============================================================================
#pragma once

#include <driver/i2c_master.h>

#include <cstddef>
#include <cstdint>

namespace bq27220 {

// BQ27220 默认 I2C 地址
constexpr uint8_t kDefaultAddress = 0x55;

// 一次采样得到的电量信息
struct BatteryStatus {
    bool valid = false;           // false = 未检测到芯片 / 读取失败
    int  soc = -1;                // 电量百分比 0..100
    int  voltage_mv = -1;         // 电压 mV
    int  current_ma = 0;          // 瞬时电流 mA（负=放电，正=充电）
    int  avg_current_ma = 0;      // 平均电流 mA
    int  temperature_c10 = 0;     // 温度 ×10（例如 256 = 25.6℃）
    int  remaining_mah = 0;       // 剩余容量 mAh
    int  full_mah = 0;            // 满充容量 mAh
    int  design_mah = 0;          // 设计容量 mAh
    int  tte_min = 0;             // 预计剩余时间（分钟）
    uint16_t status_flags = 0;    // BatteryStatus 原始位域

    bool charging = false;        // 正在充电
    bool discharging = false;     // 正在放电
    bool full = false;            // 已充满
    bool low_power = false;       // 低电量（<20%）
};

class Bq27220 {
public:
    explicit Bq27220(i2c_master_bus_handle_t i2c_bus, uint8_t addr = kDefaultAddress);
    ~Bq27220();

    Bq27220(const Bq27220&) = delete;
    Bq27220& operator=(const Bq27220&) = delete;

    // 探测芯片是否存在。可安全重复调用（例如拔插电池后重新探测）。
    bool Probe();

    bool IsPresent() const { return present_; }

    // 读取一次完整状态。返回 false 表示本次读取失败。
    bool Read(BatteryStatus& out);

    // 最近一次成功读取的缓存（从未成功过则 valid=false）。
    const BatteryStatus& last() const { return last_; }

    // Board::GetBatteryLevel() 形态：level 0..100，并输出充电/放电状态。
    bool GetLevel(int& level, bool& charging, bool& discharging);

    // 打印一行诊断日志
    void Log() const;

private:
    bool     ReadRaw(uint8_t cmd, uint8_t* buf, size_t len);
    uint16_t ReadWord(uint8_t cmd);
    int16_t  ReadSignedWord(uint8_t cmd);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t addr_ = kDefaultAddress;
    bool present_ = false;
    bool dev_added_ = false;
    BatteryStatus last_{};
};

}  // namespace bq27220
