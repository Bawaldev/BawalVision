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

static const char *TAG = "MAIN";

#define TELEMETRY_QUEUE_LEN 5
#define LCD_TASK_STACK_SIZE 4096
#define WIFI_TASK_STACK_SIZE 4096
#define HTTP_STREAM_STACK_SIZE 4096
#define TEST_PRODUCER_STACK_SIZE 3072

/* ============================================================
 *  LED CONFIGURATION
 *  ------------------------------------------------------------
 *  Sesuaikan daftar di bawah dengan board-mu:
 *    - WS2812_LED_GPIO  : GPIO WS2812 onboard (biasanya 48 di S3)
 *    - DISABLE_LED_PINS : semua pin yang mungkin dipakai LED hijau
 *
 *  Catatan: LED "power" yang hardwired ke 3.3V TIDAK BISA
 *  dimatikan lewat software. Kalau setelah kode ini LED hijau
 *  masih nyala, itu LED power → tutup dengan selotip / potong
 *  jalurnya secara fisik.
 * ============================================================ */
#define WS2812_LED_GPIO      GPIO_NUM_48

static const int s_disable_led_pins[] = {
    2,   // LED onboard klasik (DevKit v1, Super Mini, dsb.)
    21,  // XIAO ESP32S3 user LED
    38,  // beberapa board S3
    39,  // beberapa board S3
    47,  // beberapa board S3
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

static TaskHandle_t s_camera_task_handle = NULL;
static void disable_indicator_pin(int pin) {
    if (pin == WS2812_LED_GPIO) {
        return; 
    }

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_pullup_dis(pin);
    gpio_pulldown_dis(pin);

    gpio_set_level(pin, 0);
}

static void disable_all_indicator_leds(void) {
    const size_t n = sizeof(s_disable_led_pins) / sizeof(s_disable_led_pins[0]);
    for (size_t i = 0; i < n; ++i) {
        disable_indicator_pin(s_disable_led_pins[i]);
    }
    ESP_LOGI(TAG, "Disabled %u candidate indicator LED pins", (unsigned)n);
}

/* ============================================================
 *  Helper: WS2812 single LED → pure white
 * ============================================================ */
static void set_single_led_white(uint8_t brightness) {
    /* 1. Pastikan semua LED indikator mati dulu */
    disable_all_indicator_leds();

    /* 2. Drive single WS2812 RGB LED (GPIO48) dengan pure white */
    gpio_reset_pin(WS2812_LED_GPIO);
    gpio_set_direction(WS2812_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WS2812_LED_GPIO, 0);
    esp_rom_delay_us(200); // WS2812 reset latch

    /* Standard 24-bit GRB payload (Green, Red, Blue) */
    const uint8_t grb_data[3] = {brightness, brightness, brightness};
    const uint32_t mask = (1UL << (WS2812_LED_GPIO - 32)); // GPIO48 di out1 bit 16

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    for (int byte_idx = 0; byte_idx < 3; ++byte_idx) {
        uint8_t byte_val = grb_data[byte_idx];
        for (int bit = 7; bit >= 0; --bit) {
            if ((byte_val >> bit) & 0x01) {
                /* Bit '1': HIGH ~750 ns, LOW ~450 ns */
                GPIO.out1_w1ts.val = mask;
                uint32_t start = esp_cpu_get_cycle_count();
                while ((esp_cpu_get_cycle_count() - start) < 165) {}

                GPIO.out1_w1tc.val = mask;
                start = esp_cpu_get_cycle_count();
                while ((esp_cpu_get_cycle_count() - start) < 95) {}
            } else {
                /* Bit '0': HIGH ~300 ns, LOW ~900 ns */
                GPIO.out1_w1ts.val = mask;
                uint32_t start = esp_cpu_get_cycle_count();
                while ((esp_cpu_get_cycle_count() - start) < 55) {}

                GPIO.out1_w1tc.val = mask;
                start = esp_cpu_get_cycle_count();
                while ((esp_cpu_get_cycle_count() - start) < 195) {}
            }
        }
    }

    portEXIT_CRITICAL(&mux);

    /* Latch data (hold LOW > 150 us) */
    GPIO.out1_w1tc.val = mask;
    esp_rom_delay_us(200);

    ESP_LOGI(TAG, "Indicator LEDs disabled. WS2812 set to WHITE (brightness: %d)", brightness);
}

/* ============================================================
 *  app_main
 * ============================================================ */
void app_main(void) {
    /* Initialize LED configuration before other peripherals start */
    set_single_led_white(180);

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
    xTaskCreateStaticPinnedToCore(lcd_task, "lcd_task", LCD_TASK_STACK_SIZE, NULL, 5,
                                  s_lcd_task_stack, &s_lcd_task_buffer, 0);

    /* Dispatcher task for telemetry dispatch pinned to Core 0 */
    s_wifi_task_handle = xTaskCreateStaticPinnedToCore(
        WiFi_Server_Task, "WiFi_Server_Task", WIFI_TASK_STACK_SIZE,
        (void *)s_telemetry_queue, 5, s_wifi_task_stack, &s_wifi_task_buffer, 0);

    /* Dedicated HTTP video streamer task pinned to Core 1 */
    s_http_sender_task_handle = xTaskCreateStaticPinnedToCore(
        http_stream_sender_task, "HTTP_Stream_Task", HTTP_STREAM_STACK_SIZE,
        NULL, 4, s_http_stream_task_stack, &s_http_stream_task_buffer, 1);

    /* Telemetry producer task pinned to Core 1 */
    s_test_task_handle = xTaskCreateStaticPinnedToCore(
        test_telemetry_producer_task, "Test_Producer_Task", TEST_PRODUCER_STACK_SIZE,
        (void *)s_telemetry_queue, 3, s_test_producer_task_stack, &s_test_producer_task_buffer, 1);

    ESP_LOGI(TAG, "All system static tasks started successfully.");
}