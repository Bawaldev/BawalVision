#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "soc/gpio_struct.h"

#include "data.h"
#include "lcd_task.h"
#include "camera_task.h"
#include "wifi.h"
#include "led_strip.h"
#include "pins.h"

static const char *TAG = "MAIN";

#define LCD_TASK_STACK_SIZE 4096
#define WIFI_TASK_STACK_SIZE 4096
#define USB_PROV_STACK_SIZE 4096
#define BUTTON_LED_TASK_STACK_SIZE 4096

#define WS2812_LED_GPIO      GPIO_NUM_48

static const int s_candidate_indicator_pins[] = {
    21,  /* Secondary indicator / flash LED on S3 CAM */
    38,  /* Auxiliary indicator / backlight pin */
};

static StaticTask_t s_lcd_task_buffer;
static StackType_t s_lcd_task_stack[LCD_TASK_STACK_SIZE];

static StaticTask_t s_wifi_task_buffer;
static StackType_t s_wifi_task_stack[WIFI_TASK_STACK_SIZE];
static TaskHandle_t s_wifi_task_handle = NULL;

static StaticTask_t s_usb_prov_task_buffer;
static StackType_t s_usb_prov_task_stack[USB_PROV_STACK_SIZE];
static TaskHandle_t s_usb_prov_task_handle = NULL;

static StaticTask_t s_button_led_task_buffer;
static StackType_t s_button_led_task_stack[BUTTON_LED_TASK_STACK_SIZE];
static TaskHandle_t s_button_led_task_handle = NULL;

static portMUX_TYPE s_ws2812_mux = portMUX_INITIALIZER_UNLOCKED;

static void send_ws2812_zero_stream(gpio_num_t gpio_num) {
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) return;

    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
    esp_rom_delay_us(350);

    const bool is_high_reg = (gpio_num >= 32);
    const uint32_t mask = is_high_reg ? (1UL << (gpio_num - 32)) : (1UL << gpio_num);

    portENTER_CRITICAL(&s_ws2812_mux);
    for (int i = 0; i < 48; ++i) {
        if (is_high_reg) {
            GPIO.out1_w1ts.val = mask;
            uint32_t start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - start) < 72) {}
            GPIO.out1_w1tc.val = mask;
            start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - start) < 216) {}
        } else {
            GPIO.out_w1ts = mask;
            uint32_t start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - start) < 72) {}
            GPIO.out_w1tc = mask;
            start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - start) < 216) {}
        }
    }
    if (is_high_reg) GPIO.out1_w1tc.val = mask;
    else             GPIO.out_w1tc = mask;
    portEXIT_CRITICAL(&s_ws2812_mux);

    esp_rom_delay_us(350);
}

static void turn_off_ws2812_rmt(gpio_num_t gpio_num) {
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) return;

    led_strip_handle_t led_strip = NULL;
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,
        .max_leds = 2,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (err == ESP_OK && led_strip != NULL) {
        led_strip_clear(led_strip);
        vTaskDelay(pdMS_TO_TICKS(10));
        led_strip_del(led_strip);
    } else {
        send_ws2812_zero_stream(gpio_num);
    }

    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
    gpio_pulldown_en(gpio_num);
}

static void disable_discrete_led_pin(int pin) {
    if (pin < 0 || pin >= GPIO_NUM_MAX) return;
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_pullup_dis((gpio_num_t)pin);
    gpio_pulldown_dis((gpio_num_t)pin);
}

static void disable_all_indicator_leds(void) {
    turn_off_ws2812_rmt(GPIO_NUM_48);
    turn_off_ws2812_rmt(GPIO_NUM_38);

    const size_t num_pins = sizeof(s_candidate_indicator_pins) / sizeof(s_candidate_indicator_pins[0]);
    for (size_t i = 0; i < num_pins; ++i) {
        disable_discrete_led_pin(s_candidate_indicator_pins[i]);
    }
    ESP_LOGI(TAG, "All indicator LEDs and WS2812 RGB LEDs completely extinguished.");
}

static bool pin_conflicts_with_camera(int pin) {
    const int cam_pins[] = {
        CAM_PIN_PWDN, CAM_PIN_RESET, CAM_PIN_XCLK,
        CAM_PIN_SIOD, CAM_PIN_SIOC,
        CAM_PIN_D0, CAM_PIN_D1, CAM_PIN_D2, CAM_PIN_D3,
        CAM_PIN_D4, CAM_PIN_D5, CAM_PIN_D6, CAM_PIN_D7,
        CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK,
    };
    for (size_t i = 0; i < sizeof(cam_pins) / sizeof(cam_pins[0]); i++) {
        if (cam_pins[i] >= 0 && cam_pins[i] == pin) return true;
    }
    return false;
}

static void button_led_task(void *pvParameters) {
    (void)pvParameters;

    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_TRIGGER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_FLASH),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(PIN_LED_FLASH, 0);

    ESP_LOGI(TAG, "Button-LED task active: GPIO %d (btn) -> GPIO %d (LED)",
             PIN_BTN_TRIGGER, PIN_LED_FLASH);

    int stable = 1, last = 1, count = 0;
    for (;;) {
        int raw = gpio_get_level(PIN_BTN_TRIGGER);
        if (raw == last) {
            if (count < 3) count++;
            if (count == 3 && raw != stable) {
                stable = raw;
                gpio_set_level(PIN_LED_FLASH, (stable == 0) ? 1 : 0);
            }
        } else {
            last = raw;
            count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    usb_serial_jtag_driver_config_t usbjtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usbjtag_cfg.rx_buffer_size = 1024;
    usbjtag_cfg.tx_buffer_size = 1024;
    (void)usb_serial_jtag_driver_install(&usbjtag_cfg);

    disable_all_indicator_leds();

    if (pin_conflicts_with_camera(PIN_LED_FLASH)) {
        ESP_LOGE(TAG, "PIN_LED_FLASH (%d) conflicts with a camera pin!", PIN_LED_FLASH);
    }
    if (pin_conflicts_with_camera(PIN_BTN_TRIGGER)) {
        ESP_LOGE(TAG, "PIN_BTN_TRIGGER (%d) conflicts with a camera pin!", PIN_BTN_TRIGGER);
    }

    esp_log_level_set("camera", ESP_LOG_INFO);
    esp_log_level_set("esp_camera", ESP_LOG_INFO);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Initializing Camera Driver (esp_camera)...");
    ret = camera_task_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Camera Driver: %s", esp_err_to_name(ret));
    } else {
        // Core 1 is dedicated to camera capture DMA (priority 5)
        if (camera_task_start(5, 1) == NULL) {
            ESP_LOGE(TAG, "Failed to start camera_task on Core 1");
        } else {
            ESP_LOGI(TAG, "Camera capture task started on Core 1");
        }
    }

    ESP_LOGI(TAG, "Starting 128x160 SPI TFT LVGL Task (Static)...");
    TaskHandle_t lcd_task_handle = xTaskCreateStaticPinnedToCore(
        lcd_task, "lcd_task", LCD_TASK_STACK_SIZE, NULL, 5,
        s_lcd_task_stack, &s_lcd_task_buffer, 0);
    if (lcd_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create lcd_task");
        return;
    }

    ESP_LOGI(TAG, "Connecting to Wi-Fi Network...");
    wifi_init_sta();
    esp_err_t stream_server_err = wifi_http_stream_server_start();
    if (stream_server_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MJPEG HTTP server: %s",
                 esp_err_to_name(stream_server_err));
    }

    s_wifi_task_handle = xTaskCreateStaticPinnedToCore(
        WiFi_Server_Task, "WiFi_Server_Task", WIFI_TASK_STACK_SIZE,
        NULL, 4, s_wifi_task_stack, &s_wifi_task_buffer, 0);
    if (s_wifi_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi_Server_Task");
        return;
    }

    s_usb_prov_task_handle = xTaskCreateStaticPinnedToCore(
        usb_serial_provisioning_task, "USB_Prov_Task", USB_PROV_STACK_SIZE,
        NULL, 3, s_usb_prov_task_stack, &s_usb_prov_task_buffer, 0);
    if (s_usb_prov_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create USB_Prov_Task");
        return;
    }

    s_button_led_task_handle = xTaskCreateStaticPinnedToCore(
        button_led_task, "Button_LED_Task", BUTTON_LED_TASK_STACK_SIZE,
        NULL, 2, s_button_led_task_stack, &s_button_led_task_buffer, 0);
    if (s_button_led_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create Button_LED_Task");
        return;
    }

    ESP_LOGI(TAG, "All system static tasks started successfully.");
}
