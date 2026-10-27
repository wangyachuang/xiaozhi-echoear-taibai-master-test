#include "magai_wifi.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "led/circular_strip.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "weather.h"
#include "weather_display_new.h"
#include "app_animation.h"
extern "C" {
#include "test_c_array.h"
}

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <esp_lcd_nv303b.h>


#define TAG "magai_wifi"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);
LV_FONT_DECLARE(lv_font_montserrat_48);

class NV303bDisplay : public SpiLcdDisplay {
private:
    lv_obj_t* weather_clock_container_ = nullptr;
    lv_obj_t* city_label_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* weather_icon_img_ = nullptr;   // 图像图标对象
    lv_obj_t* weather_text_label_ = nullptr;
    bool weather_clock_mode_ = false;
    
    void SetupWeatherClockUI() {
        DisplayLockGuard lock(this);
        
        // 创建天气时钟容器
        weather_clock_container_ = lv_obj_create(lv_scr_act());
        lv_obj_set_size(weather_clock_container_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(weather_clock_container_, current_theme_.background, 0);
        lv_obj_set_style_border_width(weather_clock_container_, 0, 0);
        lv_obj_add_flag(weather_clock_container_, LV_OBJ_FLAG_HIDDEN);
        
        // 创建城市标签（顶部）
        city_label_ = lv_label_create(weather_clock_container_);
        lv_obj_set_style_text_font(city_label_, fonts_.text_font, 0);
        lv_obj_set_style_text_color(city_label_, current_theme_.text, 0);
        lv_label_set_text(city_label_, "武汉市");
        lv_obj_align(city_label_, LV_ALIGN_TOP_MID, 0, 10);
        
        // 创建时间标签（中间大字）
        time_label_ = lv_label_create(weather_clock_container_);
        lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(time_label_, current_theme_.text, 0);
        lv_label_set_text(time_label_, "11:04");
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, -30);
        
        // 创建温度标签
        temp_label_ = lv_label_create(weather_clock_container_);
        lv_obj_set_style_text_font(temp_label_, fonts_.text_font, 0);
        lv_obj_set_style_text_color(temp_label_, current_theme_.text, 0);
        lv_label_set_text(temp_label_, "25°C");
        lv_obj_align(temp_label_, LV_ALIGN_RIGHT_MID, -20, 0);  // 右侧中央位置
        
        // 使用新的天气图标显示模块初始化天气图标
        esp_err_t ret = weather_icon_new_init(weather_clock_container_);
        if (ret == ESP_OK) {
            weather_icon_img_ = weather_icon_new_get_obj();
            if (weather_icon_img_) {
                // 设置固定尺寸和位置 - 调整到屏幕左侧中央位置
                lv_obj_set_size(weather_icon_img_, 60, 60);  // 减小尺寸避免重叠
                lv_obj_align(weather_icon_img_, LV_ALIGN_LEFT_MID, 20, 0);  // 左侧中央位置
                lv_img_set_zoom(weather_icon_img_, 255);  // 设置缩放
                
                // 确保图标可见性设置
                lv_obj_clear_flag(weather_icon_img_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_img_opa(weather_icon_img_, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(weather_icon_img_, 0, 0);
                lv_obj_set_style_border_width(weather_icon_img_, 0, 0);
                
                // 设置背景透明
                lv_obj_set_style_bg_opa(weather_icon_img_, LV_OPA_TRANSP, 0);
                
                ESP_LOGI("MAGAI_WIFI", "Weather icon positioned at LEFT_MID(20, 0) with size 60x60");
                
                // 获取实际位置信息
                lv_coord_t icon_x = lv_obj_get_x(weather_icon_img_);
                lv_coord_t icon_y = lv_obj_get_y(weather_icon_img_);
                lv_coord_t icon_w = lv_obj_get_width(weather_icon_img_);
                lv_coord_t icon_h = lv_obj_get_height(weather_icon_img_);
                ESP_LOGI("MAGAI_WIFI", "Weather icon actual position: (%ld, %ld), size: %ldx%ld", 
                         (long)icon_x, (long)icon_y, (long)icon_w, (long)icon_h);
            }
        } else {
            ESP_LOGE("MAGAI_WIFI", "Failed to initialize weather icon display");
        }
        
        // 创建天气文字标签
        weather_text_label_ = lv_label_create(weather_clock_container_);
        lv_obj_set_style_text_font(weather_text_label_, fonts_.text_font, 0);
        lv_obj_set_style_text_color(weather_text_label_, current_theme_.text, 0);
        lv_label_set_text(weather_text_label_, "晴");
        lv_obj_align(weather_text_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
    
public:
    NV303bDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy, 
                    {
                        .text_font = &font_puhui_16_4,
                        .icon_font = &font_awesome_16_4,
                        .emoji_font = font_emoji_32_init(),
                    }) {
        SetupWeatherClockUI();
    }
    
    // 更新天气时钟显示
    void UpdateWeatherClock(const std::string& city, const std::string& time, 
                           float temperature, const std::string& weather_text,
                           const std::string& weather_code = "") {
        DisplayLockGuard lock(this);
        
        if (city_label_) {
            lv_label_set_text(city_label_, city.c_str());
        }
        
        if (time_label_) {
            lv_label_set_text(time_label_, time.c_str());
        }
        
        if (temp_label_) {
            char temp_str[16];
            snprintf(temp_str, sizeof(temp_str), "%.1f°C", temperature);
            lv_label_set_text(temp_label_, temp_str);
        }
        
        // 更新天气图标 - 使用新的天气图标显示模块
        if (!weather_code.empty()) {
            // 使用天气代码更新图标
            weather_icon_new_update(weather_code.c_str());
            
            // 添加详细的调试信息检查天气图标状态
            if (weather_icon_img_) {
                lv_coord_t icon_x = lv_obj_get_x(weather_icon_img_);
                lv_coord_t icon_y = lv_obj_get_y(weather_icon_img_);
                lv_coord_t icon_w = lv_obj_get_width(weather_icon_img_);
                lv_coord_t icon_h = lv_obj_get_height(weather_icon_img_);
                bool is_hidden = lv_obj_has_flag(weather_icon_img_, LV_OBJ_FLAG_HIDDEN);
                lv_opa_t opa = lv_obj_get_style_opa(weather_icon_img_, 0);
                ESP_LOGI("MAGAI_WIFI", "Weather icon status: pos=(%ld,%ld), size=%ldx%ld, hidden=%s, opa=%d", 
                         (long)icon_x, (long)icon_y, (long)icon_w, (long)icon_h, 
                         is_hidden ? "true" : "false", opa);
                
                // 强制设置图标可见并移到前景
                lv_obj_clear_flag(weather_icon_img_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(weather_icon_img_);
                lv_obj_invalidate(weather_icon_img_);
                
                // 确保背景透明
                lv_obj_set_style_bg_opa(weather_icon_img_, LV_OPA_TRANSP, 0);
            }
            
            // 添加调试信息检查其他UI元素位置
            if (temp_label_) {
                lv_coord_t temp_x = lv_obj_get_x(temp_label_);
                lv_coord_t temp_y = lv_obj_get_y(temp_label_);
                lv_coord_t temp_w = lv_obj_get_width(temp_label_);
                lv_coord_t temp_h = lv_obj_get_height(temp_label_);
                ESP_LOGI("MAGAI_WIFI", "Temp label position: (%ld, %ld), size: %ldx%ld", 
                         (long)temp_x, (long)temp_y, (long)temp_w, (long)temp_h);
            }
            if (weather_text_label_) {
                lv_coord_t text_x = lv_obj_get_x(weather_text_label_);
                lv_coord_t text_y = lv_obj_get_y(weather_text_label_);
                ESP_LOGI("MAGAI_WIFI", "Weather text position: (%ld, %ld)", (long)text_x, (long)text_y);
            }
        }
        
        if (weather_text_label_) {
            lv_label_set_text(weather_text_label_, weather_text.c_str());
        }
    };
    
    // 切换到天气时钟模式
    void ShowWeatherClock(bool show) {
        DisplayLockGuard lock(this);
        weather_clock_mode_ = show;
        
        ESP_LOGI("MAGAI_WIFI", "ShowWeatherClock: %s, container: %p", show ? "true" : "false", weather_clock_container_);
        
        if (show) {
            // 隐藏常规UI元素
            if (container_) {
                lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
            }
            if (status_bar_) {
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            }
            
            // 显示天气时钟UI
            if (weather_clock_container_) {
                lv_obj_clear_flag(weather_clock_container_, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI("MAGAI_WIFI", "Weather clock container is now visible");
            }
        } else {
            // 隐藏天气时钟UI
            if (weather_clock_container_) {
                lv_obj_add_flag(weather_clock_container_, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI("MAGAI_WIFI", "Weather clock container is now hidden");
            }
            
            // 显示常规UI元素
            if (container_) {
                lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
            }
            if (status_bar_) {
                lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    
    // 检查是否处于天气时钟模式
    bool IsWeatherClockMode() const {
        return weather_clock_mode_;
    }
};

class magai_wifi : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_i80_bus_handle_t display_i80_bus_;
    NV303bDisplay* display_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    iot::Weather* weather_ = nullptr;
    bool idle_mode_ = false;
    // 以下变量已不再使用
    // int idle_counter_ = 0;
    // const int kIdleTimeoutSeconds = 30; // 30秒无操作后进入天气时钟模式
    esp_timer_handle_t weather_clock_timer_ = nullptr; // 定时器句柄，用于自动更新时间

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_config = {};
        i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_config.glitch_ignore_cnt = 7;
        i2c_bus_config.i2c_port = 0;
        i2c_bus_config.sda_io_num = DISPLAY_SDA_PIN;
        i2c_bus_config.scl_io_num = DISPLAY_SCL_PIN;
        i2c_bus_config.flags.enable_internal_pullup = true;
    
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));
    
        i2c_device_config_t i2c_dev_conf = {};
        i2c_master_dev_handle_t i2c_dev_handle = NULL;
        i2c_dev_conf.dev_addr_length = I2C_ADDR_BIT_LEN_7;  // 7位地址
        i2c_dev_conf.scl_speed_hz = 100000;
        i2c_dev_conf.device_address = 0X15;
        i2c_master_bus_add_device(i2c_bus, &i2c_dev_conf, &i2c_dev_handle);
    }

    void InitializeButtons() {
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }
    void InitializeNv303bDisplay() {
        esp_lcd_panel_io_handle_t pannel_io = nullptr;
        esp_lcd_panel_handle_t panel_handle = nullptr;

        esp_lcd_i80_bus_config_t bus_config = {};
        bus_config.clk_src = LCD_CLK_SRC_DEFAULT;
        bus_config.dc_gpio_num = DISPLAY_PIN_NUM_DC;
        bus_config.wr_gpio_num = DISPLAY_PIN_NUM_PCLK;
        bus_config.data_gpio_nums[0] = DISPLAY_PIN_NUM_DATA0;
        bus_config.data_gpio_nums[1] = DISPLAY_PIN_NUM_DATA1;
        bus_config.data_gpio_nums[2] = DISPLAY_PIN_NUM_DATA2;
        bus_config.data_gpio_nums[3] = DISPLAY_PIN_NUM_DATA3;
        bus_config.data_gpio_nums[4] = DISPLAY_PIN_NUM_DATA4;
        bus_config.data_gpio_nums[5] = DISPLAY_PIN_NUM_DATA5;
        bus_config.data_gpio_nums[6] = DISPLAY_PIN_NUM_DATA6;
        bus_config.data_gpio_nums[7] = DISPLAY_PIN_NUM_DATA7;
        bus_config.bus_width = 8;
        bus_config.max_transfer_bytes = DISPLAY_WIDTH * 100 * sizeof(uint16_t);
        bus_config.dma_burst_size = DISPLAY_DMA_BURST_SIZE;
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &display_i80_bus_));

        esp_lcd_panel_io_i80_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_PIN_NUM_CS;
        io_config.pclk_hz = DISPLAY_LCD_PIXEL_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.dc_levels.dc_idle_level  = 0;
        io_config.dc_levels.dc_cmd_level   = 0;
        io_config.dc_levels.dc_dummy_level = 0;
        io_config.dc_levels.dc_data_level  = 1;
        io_config.flags.swap_color_bytes = 0;
        io_config.lcd_cmd_bits = DISPLAY_LCD_CMD_BITS;
        io_config.lcd_param_bits = DISPLAY_LCD_PARAM_BITS;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(display_i80_bus_, &io_config, &pannel_io));

        ESP_LOGI(TAG, "Install LCD driver of st7789");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_PIN_NUM_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_nv303b(pannel_io, &panel_config, &panel_handle));

        esp_lcd_panel_reset(panel_handle);
        esp_lcd_panel_init(panel_handle);

        esp_lcd_panel_invert_color(panel_handle, true);
        esp_lcd_panel_swap_xy(panel_handle, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_handle, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_set_gap(panel_handle, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        display_ = new NV303bDisplay(pannel_io, panel_handle,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

    }
    // 物联网初始化，添加对 AI 可见设备
    void MountWeatherSpiffs() {
        // Mount the weather partition to /spiffs
        esp_vfs_spiffs_conf_t conf = {
            .base_path = "/spiffs",
            .partition_label = "weather",
            .max_files = 5,
            .format_if_mount_failed = false,  // Don't format, we have pre-created images
        };
        esp_err_t ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount weather SPIFFS partition: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Weather SPIFFS partition mounted successfully at /spiffs");
            
            // Log SPIFFS info
            size_t total = 0, used = 0;
            ret = esp_spiffs_info("weather", &total, &used);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "SPIFFS: %d KB total, %d KB used", total / 1024, used / 1024);
            }
        }
    }
    
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Lamp"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        
        // 初始化天气服务
        weather_ = new iot::Weather();
        weather_->SetUserData(this); // 设置用户数据为当前对象，用于回调
        thing_manager.AddThing(weather_);
    }
    
    // 初始化天气时钟定时器
    void InitializeWeatherClockTimer() {
        const esp_timer_create_args_t weather_clock_timer_args = {
            .callback = [](void* arg) {
                magai_wifi* board = static_cast<magai_wifi*>(arg);
                if (board && board->idle_mode_) {
                    board->UpdateWeatherClock();
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "weather_clock_timer",
            .skip_unhandled_events = true
        };
        
        esp_timer_create(&weather_clock_timer_args, &weather_clock_timer_);
        // 每分钟更新一次时间显示
        esp_timer_start_periodic(weather_clock_timer_, 60 * 1000000); // 60秒 * 1000000微秒
    }
    
    // 检查设备状态并更新天气时钟显示
    void CheckDeviceStateAndUpdateWeatherClock() {
        DeviceState current_state = Application::GetInstance().GetDeviceState();
        
        // 添加调试信息
        ESP_LOGI("MAGAI_WIFI", "Device state: %d, idle_mode: %s", current_state, idle_mode_ ? "true" : "false");
        
        // 如果设备处于空闲状态，显示天气时钟
        if (current_state == kDeviceStateIdle && !idle_mode_) {
            idle_mode_ = true;
            ESP_LOGI("MAGAI_WIFI", "Entering weather clock mode");
            display_->ShowWeatherClock(true);
             UpdateWeatherClock(); // 立即更新一次天气时钟显示
        } 
        // 如果设备不处于空闲状态，隐藏天气时钟
        else if (current_state != kDeviceStateIdle && idle_mode_) {
            idle_mode_ = false;
            ESP_LOGI("MAGAI_WIFI", "Exiting weather clock mode");
            display_->ShowWeatherClock(false);
        }
    }
    
    // 更新天气时钟显示
    void UpdateWeatherClock() {
        // 首先检查设备状态
        CheckDeviceStateAndUpdateWeatherClock();
        
        // 如果处于天气时钟模式，更新显示
        if (idle_mode_) {
            // 检查天气数据是否已就绪
            bool weather_ready = weather_ && weather_->IsDataReady();
            ESP_LOGI(TAG, "天气时钟模式检查: weather_=%p, IsDataReady=%s", weather_, weather_ready ? "true" : "false");
            if (weather_ready) {
            // 获取当前时间
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            
            // 格式化时间字符串
            std::stringstream time_ss;
            time_ss << std::setfill('0') << std::setw(2) << timeinfo.tm_hour << ":" 
                    << std::setfill('0') << std::setw(2) << timeinfo.tm_min;
            
            // 获取天气数据
            // 由于无法直接访问properties_，我们需要通过Thing类的GetStateJson方法获取状态
            cJSON *state_json = cJSON_Parse(weather_->GetStateJson().c_str());
            std::string city = "未知";
            float temperature = 0.0f;
            std::string weather_text = "未知";
            
            if (state_json) {
                // 从state_json中获取state对象
                cJSON *state = cJSON_GetObjectItem(state_json, "state");
                if (state) {
                    cJSON *city_json = cJSON_GetObjectItem(state, "city");
                    if (city_json && cJSON_IsString(city_json)) {
                        city = city_json->valuestring;
                    }
                    
                    cJSON *temp_json = cJSON_GetObjectItem(state, "temperature");
                    if (temp_json && cJSON_IsNumber(temp_json)) {
                        temperature = temp_json->valuedouble;
                    }
                    
                    cJSON *weather_json = cJSON_GetObjectItem(state, "weather");
                    if (weather_json && cJSON_IsString(weather_json)) {
                        weather_text = weather_json->valuestring;
                    }
                }
                
                cJSON_Delete(state_json);
            }
            
            // 获取天气代码
            std::string weather_code = "";
            
            if (weather_) {
                weather_code = weather_->GetWeatherCode();
                
                // 如果天气数据有效，主动更新一次天气数据
                if (city == "未知" || weather_text == "未知" || temperature == 0.0f) {
                    ESP_LOGI(TAG, "天气数据无效，尝试更新天气数据");
                    // 创建任务来更新天气，避免阻塞主线程
                    xTaskCreate(
                        [](void* arg) {
                            iot::Weather* w = static_cast<iot::Weather*>(arg);
                            // 先尝试通过公网IP自动获取城市
                            if (w->AutoDetectCity()) {
                                ESP_LOGI(TAG, "已通过公网IP自动获取城市: %s", w->GetCity().c_str());
                            } else {
                                ESP_LOGI(TAG, "无法通过公网IP获取城市，使用默认城市");
                            }
                            // 更新天气数据
                            w->UpdateWeather();
                            vTaskDelete(NULL);
                        },
                        "weather_update_task",   // 任务名称
                        8192,                    // 堆栈大小
                        weather_,                // 任务参数
                        5,                       // 任务优先级
                        NULL                     // 任务句柄
                    );
                }
            }
            
            // 更新天气时钟显示
            display_->UpdateWeatherClock(city, time_ss.str(), temperature, weather_text, weather_code);
            
            // 记录日志，便于调试
            ESP_LOGI(TAG, "天气时钟UI已更新: 城市=%s, 时间=%s, 温度=%.1f°C, 天气=%s, 天气代码=%s", 
                     city.c_str(), time_ss.str().c_str(), temperature, weather_text.c_str(), weather_code.c_str());
            }
            else {
                // 获取当前时间
                time_t now;
                struct tm timeinfo;
                time(&now);
                localtime_r(&now, &timeinfo);
                
                // 格式化时间字符串
                std::stringstream time_ss;
                time_ss << std::setfill('0') << std::setw(2) << timeinfo.tm_hour << ":" 
                        << std::setfill('0') << std::setw(2) << timeinfo.tm_min;
                
                // 显示加载中的提示
                display_->UpdateWeatherClock("加载中...", time_ss.str(), 0.0f, "正在获取天气数据", "");
                ESP_LOGI(TAG, "天气数据未就绪，显示加载中提示");
            }
        }
    }
    
    // 检查是否有用户活动（按钮按下等）- 此方法已不再使用，因为我们现在使用定时器来定期检查设备状态
    /*
    bool HasUserActivity() {
        // 检查是否有用户活动，可以通过Application类的设备状态来判断
        // 或者检查按钮状态
        DeviceState current_state = Application::GetInstance().GetDeviceState();
        bool is_active = (current_state != kDeviceStateIdle); // 如果不是空闲状态，则认为有活动
        
        // 由于Button类没有IsPressed方法，我们暂时只通过设备状态判断
        return is_active;
        // 如果需要检查按钮状态，需要在Button类中添加IsPressed方法
        // 或者使用其他方式检查按钮状态
    }
    */

public:
    magai_wifi() :
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
            InitializeButtons();
            InitializeIot();
            InitializeI2c();
            InitializeNv303bDisplay();
            GetBacklight()->RestoreBrightness();
            
            // 挂载天气图标 SPIFFS 分区
            MountWeatherSpiffs();
            
            // PNG解码器测试已移动到Application::Start方法中，在小智AI框架完全初始化后执行
            
            // 初始化天气图标MMAP文件系统
            esp_err_t ret = app_animation_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize weather animation system: %s", esp_err_to_name(ret));
            }
            
            // 初始状态检查，如果是空闲状态则显示天气时钟
            CheckDeviceStateAndUpdateWeatherClock();
            
            // 使用定时器定期更新天气时钟UI
            InitializeWeatherClockTimer();
            
            // 启动C数组格式天气图标测试
            start_c_array_test();
            ESP_LOGI(TAG, "C数组格式天气图标测试已启动");
    }
    
    ~magai_wifi() {
        // 清理定时器资源
        if (weather_clock_timer_ != nullptr) {
            esp_timer_stop(weather_clock_timer_);
            esp_timer_delete(weather_clock_timer_);
            weather_clock_timer_ = nullptr;
        }
        
        delete weather_;
    }

    // 自定义Led类，重写OnStateChanged方法，在设备状态变化时更新天气时钟UI
    class MagaiLed : public CircularStrip {
    private:
        magai_wifi* board_;
        
    public:
        MagaiLed(int gpio_num, int led_num, magai_wifi* board) 
            : CircularStrip(static_cast<gpio_num_t>(gpio_num), led_num), board_(board) {}
        
        virtual void OnStateChanged() override {
            // 首先调用父类的OnStateChanged方法
            CircularStrip::OnStateChanged();
            
            // 检查设备状态并更新天气时钟显示
            board_->CheckDeviceStateAndUpdateWeatherClock();
            
            // 如果处于空闲状态，更新一次天气时钟显示
            if (board_->idle_mode_) {
                // 获取当前设备状态
                DeviceState current_state = Application::GetInstance().GetDeviceState();
                
                // 如果设备状态为空闲，触发天气数据更新
                if (current_state == kDeviceStateIdle && board_->weather_ != nullptr) {
                    // 创建任务来更新天气，避免阻塞主线程
                    xTaskCreate(
                        [](void* arg) {
                            iot::Weather* w = static_cast<iot::Weather*>(arg);
                            // 先尝试通过公网IP自动获取城市
                            if (w->AutoDetectCity()) {
                                ESP_LOGI(TAG, "已通过公网IP自动获取城市: %s", w->GetCity().c_str());
                            } else {
                                ESP_LOGI(TAG, "无法通过公网IP获取城市，使用默认城市");
                            }
                            // 更新天气数据
                            w->UpdateWeather();
                            
                            // 添加延迟，确保数据已就绪
                            vTaskDelay(pdMS_TO_TICKS(200));
                            
                            // 通知主线程更新UI
                            magai_wifi* board = static_cast<magai_wifi*>(w->GetUserData());
                            if (board && w->IsDataReady()) {
                                board->UpdateWeatherClock();
                                ESP_LOGI(TAG, "天气数据已就绪，触发UI更新");
                            }
                            
                            vTaskDelete(NULL);
                        },
                        "weather_update_init",   // 任务名称
                        8192,                    // 堆栈大小
                        board_->weather_,        // 任务参数
                        5,                       // 任务优先级
                        NULL                     // 任务句柄
                    );
                    ESP_LOGI(TAG, "在设备状态变为idle后触发天气数据更新");
                }
                
                // 不再直接调用UpdateWeatherClock，而是在天气数据就绪后由任务触发更新
                // 如果天气数据已就绪，则可以立即更新UI
                if (board_->weather_ && board_->weather_->IsDataReady()) {
                    board_->UpdateWeatherClock();
                }
            }
        }
    };
    
    virtual Led* GetLed() override {
        static MagaiLed led(BUILTIN_LED_GPIO, BUILTIN_LED_NUM, this);
        return &led;
    }
    
    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(magai_wifi);
