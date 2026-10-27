/**
 * 天气图标实现文件
 */

#include "lvgl.h"

// 声明天气图标
LV_IMG_DECLARE(weather_icon_sunny);
LV_IMG_DECLARE(weather_icon_cloudy);
LV_IMG_DECLARE(weather_icon_partly_cloudy);
LV_IMG_DECLARE(weather_icon_rainy);
LV_IMG_DECLARE(weather_icon_snowy);
LV_IMG_DECLARE(weather_icon_foggy);
LV_IMG_DECLARE(weather_icon_thunderstorm);

// 天气代码到图标的映射表
const lv_img_dsc_t* weather_code_to_icon[] = {
    &weather_icon_sunny,         // 0 - 晴天
    &weather_icon_cloudy,        // 1 - 多云
    &weather_icon_partly_cloudy, // 2 - 局部多云
    &weather_icon_rainy,         // 3 - 雨天
    &weather_icon_snowy,         // 4 - 雪天
    &weather_icon_foggy,         // 5 - 雾天
    &weather_icon_thunderstorm   // 6 - 雷暴
};