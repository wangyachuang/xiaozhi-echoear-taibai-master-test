/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "lv_demos.h"

#include "motor_ctrl.h"
#include "iot_button.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "espnow.h"
#include "espnow_ctrl.h"
#include "espnow_utils.h"

static const char *TAG = "rotating_base";

#define UART_ECHO_TXD (GPIO_NUM_21)
#define UART_ECHO_RXD (GPIO_NUM_20)
#define UART_ECHO_RTS (-1)
#define UART_ECHO_CTS (-1)

#define ECHO_UART_PORT_NUM      (1)
#define ECHO_UART_BAUD_RATE     (115200)
#define UART_CMD_TASK_STACK_SIZE    (2048)
#define BUF_SIZE (1024)

#define BUTTON_BOOT_GPIO_NUM    (GPIO_NUM_9)
#define POWER_LED_GPIO_NUM      (GPIO_NUM_2)

#define UART_FRAME_HEAD_H       (0xAA)
#define UART_FRAME_HEAD_L       (0x55)
#define UART_FRAME_HEADER_LEN   (2)
#define UART_FRAME_MIN_LEN      (5) // Minimum: header + length + command + checksum

// Declare external font
extern const lv_font_t lexin_font;

void lcd_display_init(void)
{
    bsp_base_display_start();
    bsp_display_lock(0);

    // Set screen background to pure black
    lv_obj_t * scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_100, 0);
    
    // Disable screen scrolling
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create main container
    lv_obj_t * container = lv_obj_create(scr);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(container);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_100, 0);
    lv_obj_set_style_border_width(container, 0, 0);  // Remove border
    lv_obj_set_style_radius(container, 0, 0);        // Remove rounded corners
    lv_obj_set_style_pad_all(container, 30, 0);
    
    // Disable container scrolling
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Create main label
    lv_obj_t * label = lv_label_create(container);
    lv_label_set_text(label, "EchoEar");
    lv_obj_set_style_text_font(label, &lexin_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);  // White text
    lv_obj_set_style_text_opa(label, LV_OPA_100, 0);

    lv_obj_center(label);

    bsp_display_unlock();
}

static void uart_cmd_receive_task(void *arg)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = ECHO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    while (1) {
        if (bsp_motor_driver_is_enabled()) {
            ESP_LOGI(TAG, "Motor is working, skip uart cmd receive");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Read data from the UART
        int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            // Check frame header
            if (data[0] == 0xAA && data[1] == 0x55) {
                // Get data length
                uint8_t data_len = data[3];
                // Get command code
                uint8_t cmd = data[4];
                uint8_t calc_checksum = cmd;

                // Parse data according to command type
                if (cmd == 0x01 && len >= 8) {  // Base angle control
                    // Get data value (combine two bytes)
                    uint16_t value = (data[5] << 8) | data[6];
                    // Calculate checksum
                    calc_checksum += data[5] + data[6];
                    // printf("calc_checksum: 0x%02X\n", calc_checksum);
                    uint8_t received_checksum = data[7];

                    // Print parsing results
                    ESP_LOGI(TAG, "=== Base Angle Control Command ===");
                    ESP_LOGI(TAG, "Frame header: 0x%02X 0x%02X", data[0], data[1]);
                    ESP_LOGI(TAG, "Data length: %d", data_len);
                    ESP_LOGI(TAG, "Command: 0x%02X", cmd);
                    ESP_LOGI(TAG, "Angle value: %d", value);
                    ESP_LOGI(TAG, "Checksum: 0x%02X (calculated: 0x%02X)", received_checksum, calc_checksum);

                    // Checksum verification
                    if (calc_checksum == received_checksum) {
                        ESP_LOGI(TAG, "Checksum verification passed");
                        rotating_base_angle_control(value);
                    } else {
                        ESP_LOGW(TAG, "Checksum verification failed");
                    }
                }
                else {
                    ESP_LOGW(TAG, "Invalid command or frame length");
                }
            } else {
                ESP_LOGW(TAG, "Invalid frame header");
            }
            
            // Echo data
            uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) data, len);
        }
    }
}

static void bsp_button_init(int32_t button_gpio_num, button_cb_t button_event_cb, button_event_t event)
{
    button_config_t btn_cfg = {
        .type = BUTTON_TYPE_GPIO,
        .long_press_time = 1500,
        .short_press_time = 150,
        .gpio_button_config = {
            .gpio_num = button_gpio_num,
            .active_level = 0,
        },
    };
    button_handle_t btn = iot_button_create(&btn_cfg);
    assert(btn);
    esp_err_t err = iot_button_register_cb(btn, event, button_event_cb, (void *)button_gpio_num);
    ESP_ERROR_CHECK(err);
    ESP_LOGD(TAG, "Button %ld init success", button_gpio_num);
}

static void production_test_task(void *pvParameters)
{
    // Define angle sequence for head-shaking motion
    const int angles[] = {BASE_ANGLE_MID - 100, BASE_ANGLE_MID, BASE_ANGLE_MID + 100, BASE_ANGLE_MID};
    const int num_angles = sizeof(angles) / sizeof(angles[0]);
    
    // Execute head-shaking motion sequence
    for (int i = 0; i < num_angles; i++) {
        rotating_base_angle_control(angles[i]);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Delay 1 second between each motion
    }
    
    vTaskDelete(NULL); // Delete task after completion
}

static void single_button_event_cb(void *arg, void *data)
{
    int32_t button_id = (int32_t)data;
    switch (button_id) {
    case BUTTON_BOOT_GPIO_NUM:
        ESP_LOGI(TAG, "BUTTON_BOOT pressed");
        // Create production test task
        xTaskCreate(production_test_task, "production_test_task", 2048, NULL, 5, NULL);
        break;
    }
}

static void long_press_button_event_cb(void *arg, void *data)
{
    int32_t button_id = (int32_t)data;
    switch (button_id) {
    case BUTTON_BOOT_GPIO_NUM:
        ESP_LOGI(TAG, "BUTTON_BOOT long press");
        break;
    }
}

void bsp_single_button_init(button_cb_t button_event_cb)
{
    bsp_button_init(BUTTON_BOOT_GPIO_NUM, button_event_cb, BUTTON_SINGLE_CLICK);
    ESP_LOGD(TAG, "All single button init success.");
}

void bsp_long_press_button_init(button_cb_t button_event_cb)
{
    bsp_button_init(BUTTON_BOOT_GPIO_NUM, button_event_cb, BUTTON_LONG_PRESS_HOLD);
    ESP_LOGD(TAG, "All long press button init success.");
}

typedef enum {
    APP_ESPNOW_CTRL_INIT,
    APP_ESPNOW_CTRL_BOUND,
    APP_ESPNOW_CTRL_MAX
} app_espnow_ctrl_status_t;

static app_espnow_ctrl_status_t s_espnow_ctrl_status = APP_ESPNOW_CTRL_INIT;

static void app_wifi_init()
{
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void remote_control(int ch1_value, int ch2_value, int ch3_value, int ch4_value, int ch5_value, int ch6_value, int ch7_value, int ch8_value)
{
    static int pre_ch1_value = 0;
    if (pre_ch1_value != ch1_value) {
        // ESP_LOGI(TAG, "ch1_value: %d", ch1_value);
        if (ch1_value > 140) {
            rotating_base_angle_control(210);
        } else if (ch1_value < 40) {
            rotating_base_angle_control(105);
        } else if (ch1_value < 100 && ch1_value > 80) {
            rotating_base_angle_control(155);
        }
        pre_ch1_value = ch1_value;
    }
}

static void app_responder_ctrl_data_cb(espnow_attribute_t initiator_attribute,
                                       espnow_attribute_t responder_attribute,
                                       uint32_t status1,
                                       int status2,
                                       int lx_value,
                                       int ly_value,
                                       int rx_value,
                                       int ry_value,
                                       int channel_one_value,
                                       int channel_two_value)
{
    remote_control(lx_value + 90, ly_value + 90, rx_value + 90, ry_value + 90, status1, status2, channel_one_value, channel_two_value);
}

static void app_responder_init(void)
{
    ESP_ERROR_CHECK(espnow_ctrl_responder_bind(60 * 1000, -70, NULL));
    espnow_ctrl_responder_data(app_responder_ctrl_data_cb);
}

static void app_espnow_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    if (base != ESP_EVENT_ESPNOW) {
        return;
    }

    switch (id) {
    case ESP_EVENT_ESPNOW_CTRL_BIND: {
        espnow_ctrl_bind_info_t *info = (espnow_ctrl_bind_info_t *)event_data;
        ESP_LOGI(TAG, "bind, uuid: " MACSTR ", initiator_type: %d", MAC2STR(info->mac), info->initiator_attribute);
        printf("espnow bind success\n");
        s_espnow_ctrl_status = APP_ESPNOW_CTRL_BOUND;
        break;
    }

    case ESP_EVENT_ESPNOW_CTRL_UNBIND: {
        espnow_ctrl_bind_info_t *info = (espnow_ctrl_bind_info_t *)event_data;
        ESP_LOGI(TAG, "unbind, uuid: " MACSTR ", initiator_type: %d", MAC2STR(info->mac), info->initiator_attribute);
        printf("espnow unbind success\n");
        s_espnow_ctrl_status = APP_ESPNOW_CTRL_INIT;
        break;
    }

    default:
        break;
    }
}

static void power_led_init(void)
{
    // Configure GPIO as output mode
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << POWER_LED_GPIO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

static void power_led_set_state(bool state)
{
    gpio_set_level(POWER_LED_GPIO_NUM, state ? 1 : 0);
}

void app_main(void)
{
    espnow_storage_init();

    app_wifi_init();

    esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE);

    espnow_config_t espnow_config = ESPNOW_INIT_CONFIG_DEFAULT();
    espnow_init(&espnow_config);

    esp_event_handler_register(ESP_EVENT_ESPNOW, ESP_EVENT_ANY_ID, app_espnow_event_handler, NULL);

    app_responder_init();

    lcd_display_init();

    power_led_init();
    power_led_set_state(true);  // Turn on power LED during initialization

    bsp_single_button_init(single_button_event_cb);
    bsp_long_press_button_init(long_press_button_event_cb);

    rotating_base_angle_control_init();
    xTaskCreate(uart_cmd_receive_task, "uart_cmd_receive_task", UART_CMD_TASK_STACK_SIZE, NULL, 10, NULL);
}
