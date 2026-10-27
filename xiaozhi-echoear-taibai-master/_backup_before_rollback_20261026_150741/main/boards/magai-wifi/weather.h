#pragma once

#include <string>
#include <ctime>
#include <random>
#include "iot/thing.h"
#include "lvgl.h"
#include <esp_http_client.h>

namespace iot {

// 天气状态枚举
enum class WeatherState {
    SUNNY,
    CLOUDY,
    PARTLY_CLOUDY,
    RAINY,
    SNOWY,
    FOGGY,
    THUNDERSTORM
};

// 天气文字映射
const std::unordered_map<WeatherState, std::string> kWeatherTexts = {
    {WeatherState::SUNNY, "晴"},
    {WeatherState::CLOUDY, "多云"},
    {WeatherState::PARTLY_CLOUDY, "局部多云"},
    {WeatherState::RAINY, "雨"},
    {WeatherState::SNOWY, "雪"},
    {WeatherState::FOGGY, "雾"},
    {WeatherState::THUNDERSTORM, "雷暴"}
};

// 天气数据结构体
struct WeatherData {
    std::string city;
    float temperature;
    std::string weather;
    std::string weather_code;
};

// Weather类，继承自Thing
class Weather : public Thing {
public:
    Weather();
    ~Weather();
    
    // 更新天气信息（主要方法）
    void UpdateWeather();
    
    // 自动检测城市并更新天气
    bool AutoDetectCity();
    
    // 设置城市并更新天气
    void SetCity(const std::string& city);
    
    // 获取天气数据
    WeatherData GetWeatherData() const;
    
    // 获取天气图标对象
    const lv_img_dsc_t* GetWeatherIconObject() const;
    
    // 便捷访问方法（内部调用GetWeatherData）
    std::string GetCity() const;
    float GetTemperature() const;
    std::string GetWeather() const;
    std::string GetWeatherCode() const;
    
private:
    // 通过IP获取城市信息
    bool GetCityByIP();
    
    // 获取公网IP地址
    std::string GetPublicIP();
    
    // 启动定时更新
    void StartPeriodicUpdate();
    
    // 定时器回调函数
    static void UpdateTimerCallback(void* arg);
    
    // 天气更新任务函数
    static void WeatherUpdateTask(void* arg);
    
public:
    // 设置用户数据（用于回调）
    void SetUserData(void* user_data) { user_data_ = user_data; }
    
    // 获取用户数据
    void* GetUserData() const { return user_data_; }
    
    // 检查天气数据是否就绪
    bool IsDataReady() const { return is_data_ready_; }
    
    // 成员变量
    std::string city_;
    float temperature_;
    std::string weather_;
    std::string weather_code_; // 天气代码
    bool is_updating_;
    bool is_data_ready_; // 标记天气数据是否已准备好可以显示
    esp_timer_handle_t update_timer_;
    std::random_device rd_;
    std::mt19937 gen_;
    void* user_data_; // 用户数据指针，用于回调
};

} // namespace iot