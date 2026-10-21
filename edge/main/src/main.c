#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

// Peripheral and low-level hardware control drivers
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "soc/gpio_struct.h"

#include "data.h"
#include "lcd_task.h"
#include "camera_task.h"
#include "test.h"
#include "wifi.h"
#include "led_strip.h"
#include "pins.h"

static const char *TAG = "MAIN";

#define TELEMETRY_QUEUE_LEN 5
#define LCD_TASK_STACK_SIZE 4096
#define WIFI_TASK_STACK_SIZE 4096
#define HTTP_STREAM_STACK_SIZE 4096
#define TEST_PRODUCER_STACK_SIZE 4096
#define USB_PROV_STACK_SIZE 4096
#define BUTTON_LED_TASK_STACK_SIZE 2048

#define WS2812_LED_GPIO      GPIO_NUM_48

static const int s_candidate_indicator_pins[] = {
    21,  // Secondary indicator / flash LED on S3 CAM
    38,  // Auxiliary indicator / backlight pin
};

static QueueHandle_t s_telemetry_queue = NULL;
static StaticQueue_t s_telemetry_queue_struct;
static uint8_t s_telemetry_queue_storage[TELEMETRY_QUEUE_LEN * sizeof(telemetry_data_t)];

static StaticTask_t s_lcd_task_buffer;
static StackType_t s_lcd_task_stack[LCD_TASK_STACK_SIZE];

static StaticTask_t s_wifi_task_buffer;
static StackType_t s_wifi_task_stack[WIFI_TASK_STACK_SIZE];
static TaskHandle_t s_wifi_task_handle = NULL;

static StaticTask_t s_http_stream_task_buffer;
static StackType_t s_http_stream_task_stack[HTTP_STREAM_STACK_SIZE];
static TaskHandle_t s_http_sender_task_handle = NULL;

static StaticTask_t s_test_producer_task_buffer;
static StackType_t s_test_producer_task_stack[TEST_PRODUCER_STACK_SIZE];
static TaskHandle_t s_test_task_handle = NULL;

static StaticTask_t s_usb_prov_task_buffer;
static StackType_t s_usb_prov_task_stack[USB_PROV_STACK_SIZE];
static TaskHandle_t s_usb_prov_task_handle = NULL;

static StaticTask_t s_button_led_task_buffer;
static StackType_t s_button_led_task_stack[BUTTON_LED_TASK_STACK_SIZE];
static TaskHandle_t s_button_led_task_handle = NULL;

static TaskHandle_t s_camera_task_handle = NULL;

/* ============================================================
 *  Helper: Send WS2812 0-pulse stream (240 MHz cycle timing)
 *  T0H: ~300 ns (72 cycles @ 240MHz)
 *  T0L: ~900 ns (216 cycles @ 240MHz)
 * ============================================================ */
static void send_ws2812_zero_stream(gpio_num_t gpio_num) {
    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX) return;

    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
    esp_rom_delay_us(350);

    const bool is_high_reg = (gpio_num >= 32);
    const uint32_t mask = is_high_reg ? (1UL << (gpio_num - 32)) : (1UL << gpio_num);

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

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

    if (is_high_reg) {
        GPIO.out1_w1tc.val = mask;
    } else {
        GPIO.out_w1tc = mask;
    }

    portEXIT_CRITICAL(&mux);
    esp_rom_delay_us(350);
}

/* ============================================================
 *  Helper: Official Espressif RMT WS2812 LED Extinguish
 * ============================================================ */
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

    // Actively clamp the data line to GND (0V) with pulldown to prevent RF noise / floating re-trigger
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
    gpio_pulldown_en(gpio_num);
}

/* ============================================================
 *  Helper: Disable discrete indicator pins (Active-LOW and Active-HIGH)
 *  Configuring as High-Z Input with NO pullups/pulldowns cuts all current
 * ============================================================ */
static void disable_discrete_led_pin(int pin) {
    if (pin < 0 || pin >= GPIO_NUM_MAX) return;
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_pullup_dis((gpio_num_t)pin);
    gpio_pulldown_dis((gpio_num_t)pin);
}

static void disable_all_indicator_leds(void) {
    // 1. Clear addressable WS2812 RGB LEDs on candidate pins using official RMT driver
    turn_off_ws2812_rmt(GPIO_NUM_48);
    turn_off_ws2812_rmt(GPIO_NUM_38);

    // 2. Disable discrete indicator LEDs via High-Z isolation
    const size_t num_pins = sizeof(s_candidate_indicator_pins) / sizeof(s_candidate_indicator_pins[0]);
    for (size_t i = 0; i < num_pins; ++i) {
        disable_discrete_led_pin(s_candidate_indicator_pins[i]);
    }
    ESP_LOGI(TAG, "All indicator LEDs and WS2812 RGB LEDs completely extinguished.");
}

/* ============================================================
 *  Button-LED Monitor Task (Momentary control)
 *  Button: PIN_BTN_TRIGGER (GPIO 48, Active LOW, internal pull-up)
 *  LED:    PIN_LED_FLASH (GPIO 2, Active HIGH)
 * ============================================================ */
static void button_led_task(void *pvParameters) {
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

    for (;;) {
        // Active LOW button: 0 when pressed -> turn LED ON (1); 1 when released -> turn LED OFF (0)
        int btn_state = gpio_get_level(PIN_BTN_TRIGGER);
        gpio_set_level(PIN_LED_FLASH, (btn_state == 0) ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ============================================================
 *  app_main
 * ============================================================ */
void app_main(void) {
    /* Disable all indicator and WS2812 LEDs */
    disable_all_indicator_leds();

    /* Set camera log verbosity */
    esp_log_level_set("camera", ESP_LOG_INFO);
    esp_log_level_set("esp_camera", ESP_LOG_INFO);

    /* Initialize NVS storage */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize network interface and default event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create telemetry data queue via deterministic static allocation */
    s_telemetry_queue = xQueueCreateStatic(TELEMETRY_QUEUE_LEN, sizeof(telemetry_data_t),
                                           s_telemetry_queue_storage, &s_telemetry_queue_struct);
    if (s_telemetry_queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate Static FreeRTOS Queue");
        return;
    }

    /* Initialize OV5640 CSP camera driver */
    ESP_LOGI(TAG, "Initializing OV5640 CSP Camera Driver (esp_camera)...");
    ret = camera_task_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Camera Driver: %s", esp_err_to_name(ret));
    } else {
        s_camera_task_handle = camera_task_start(5, 1);
    }

    /* Connect to Wi-Fi station */
    ESP_LOGI(TAG, "Connecting to Wi-Fi Network...");
    wifi_init_sta();

    /* Start ST7789/ST7735 128x160 SPI TFT LVGL Dashboard Task pinned to Core 0 */
    ESP_LOGI(TAG, "Starting 128x160 SPI TFT LVGL Task (Static)...");
    TaskHandle_t lcd_task_handle = xTaskCreateStaticPinnedToCore(
        lcd_task, "lcd_task", LCD_TASK_STACK_SIZE, NULL, 5,
        s_lcd_task_stack, &s_lcd_task_buffer, 0);
    if (lcd_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create lcd_task");
        return;
    }

    /* Dispatcher task for telemetry dispatch pinned to Core 0 */
    s_wifi_task_handle = xTaskCreateStaticPinnedToCore(
        WiFi_Server_Task, "WiFi_Server_Task", WIFI_TASK_STACK_SIZE,
        (void *)s_telemetry_queue, 5, s_wifi_task_stack, &s_wifi_task_buffer, 0);
    if (s_wifi_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi_Server_Task");
        return;
    }

    /* Dedicated HTTP video streamer task pinned to Core 1 */
    s_http_sender_task_handle = xTaskCreateStaticPinnedToCore(
        http_stream_sender_task, "HTTP_Stream_Task", HTTP_STREAM_STACK_SIZE,
        NULL, 4, s_http_stream_task_stack, &s_http_stream_task_buffer, 1);
    if (s_http_sender_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP_Stream_Task");
        return;
    }

    /* Telemetry producer task pinned to Core 1 */
    s_test_task_handle = xTaskCreateStaticPinnedToCore(
        test_telemetry_producer_task, "Test_Producer_Task", TEST_PRODUCER_STACK_SIZE,
        (void *)s_telemetry_queue, 3, s_test_producer_task_stack, &s_test_producer_task_buffer, 1);
    if (s_test_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create Test_Producer_Task");
        return;
    }

    /* USB Serial / UART Wi-Fi Provisioning task pinned to Core 0 */
    s_usb_prov_task_handle = xTaskCreateStaticPinnedToCore(
        usb_serial_provisioning_task, "USB_Prov_Task", USB_PROV_STACK_SIZE,
        NULL, 3, s_usb_prov_task_stack, &s_usb_prov_task_buffer, 0);
    if (s_usb_prov_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create USB_Prov_Task");
        return;
    }

    /* Button-controlled LED task pinned to Core 0 */
    s_button_led_task_handle = xTaskCreateStaticPinnedToCore(
        button_led_task, "Button_LED_Task", BUTTON_LED_TASK_STACK_SIZE,
        NULL, 2, s_button_led_task_stack, &s_button_led_task_buffer, 0);
    if (s_button_led_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create Button_LED_Task");
        return;
    }

    ESP_LOGI(TAG, "All system static tasks started successfully.");
}