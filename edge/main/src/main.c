#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

// Driver tambahan untuk kontrol GPIO & CPU cycle timing
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

/**
 * @brief Mematikan LED indikator GPIO 2 dan membersihkan latch WS2812 di GPIO 48
 */
static void turn_off_all_leds(void) {
  // 1. Matikan indikator LED standar di GPIO 2
  gpio_reset_pin(GPIO_NUM_2);
  gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_2, 0);

  // 2. Konfigurasi GPIO 48 (WS2812 RGB LED pin)
  gpio_reset_pin(GPIO_NUM_48);
  gpio_set_direction(GPIO_NUM_48, GPIO_MODE_OUTPUT);


  // Kirim 48 bit data 0 (GRB = 0,0,0) untuk memadamkan latch warna hijau WS2812
  const uint32_t mask = (1UL << (48 - 32)); // GPIO 48 berada di register out1
  for (int i = 0; i < 48; ++i) {
    // T0H: HIGH selama ~350 ns (sekitar 80 siklus pada CPU 240 MHz)
    GPIO.out1_w1ts.val = mask;
    uint32_t start = esp_cpu_get_cycle_count();
    while ((esp_cpu_get_cycle_count() - start) < 80) {}

    // T0L: LOW selama ~900 ns (sekitar 210 siklus pada CPU 240 MHz)
    GPIO.out1_w1tc.val = mask;
    start = esp_cpu_get_cycle_count();
    while ((esp_cpu_get_cycle_count() - start) < 210) {}
  }

  // Reset pulse > 60 us agar data 0 terkunci dan lampu mati
  GPIO.out1_w1tc.val = mask;
  esp_rom_delay_us(80);

  // Tahan pin di ground (LOW) dengan pull-down aktif agar tidak menangkap noise
  gpio_set_level(GPIO_NUM_48, 0);
  gpio_pullup_dis(GPIO_NUM_48);
  gpio_pulldown_en(GPIO_NUM_48);

  ESP_LOGI(TAG, "Onboard RGB & Status LEDs disabled.");
}

static void set_single_led_white(uint8_t brightness) {
  // 1. MATIKAN LED KEDUA (GPIO 2 - Indikator)
  // Set ke INPUT Hi-Z agar baik active-low maupun active-high mati total
  gpio_reset_pin(GPIO_NUM_2);
  gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);
  gpio_pullup_dis(GPIO_NUM_2);
  gpio_pulldown_dis(GPIO_NUM_2);

  // 2. NYALAKAN HANYA 1 LED (GPIO 48) WARNA PUTIH
  gpio_reset_pin(GPIO_NUM_48);
  gpio_set_direction(GPIO_NUM_48, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_48, 0);
  esp_rom_delay_us(100); // Reset latch WS2812

  // Protokol WS2812 memakai urutan data GRB (Green, Red, Blue)
  // Untuk warna PUTIH, ketiga kanal diisi nilai yang sama
  uint8_t grb_data[3] = {brightness, brightness, brightness};

  const uint32_t mask = (1UL << (48 - 32)); // GPIO 48 berada di register out1 (48-32 = bit 16)

  // Kunci interupsi sementara agar pulsa nanodetik tidak melar
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);

  for (int byte_idx = 0; byte_idx < 3; ++byte_idx) {
    uint8_t byte_val = grb_data[byte_idx];
    for (int bit = 7; bit >= 0; --bit) {
      if ((byte_val >> bit) & 0x01) {
        // Bit '1': HIGH ~850 ns (204 cycles), LOW ~400 ns (96 cycles) pada 240 MHz
        GPIO.out1_w1ts.val = mask;
        uint32_t start = esp_cpu_get_cycle_count();
        while ((esp_cpu_get_cycle_count() - start) < 200) {}

        GPIO.out1_w1tc.val = mask;
        start = esp_cpu_get_cycle_count();
        while ((esp_cpu_get_cycle_count() - start) < 95) {}
      } else {
        // Bit '0': HIGH ~350 ns (84 cycles), LOW ~850 ns (204 cycles) pada 240 MHz
        GPIO.out1_w1ts.val = mask;
        uint32_t start = esp_cpu_get_cycle_count();
        while ((esp_cpu_get_cycle_count() - start) < 80) {}

        GPIO.out1_w1tc.val = mask;
        start = esp_cpu_get_cycle_count();
        while ((esp_cpu_get_cycle_count() - start) < 200) {}
      }
    }
  }

  // Buka kembali interupsi
  portEXIT_CRITICAL(&mux);

  // Latch data WS2812 (tahan LOW > 60 us)
  GPIO.out1_w1tc.val = mask;
  esp_rom_delay_us(80);

  ESP_LOGI(TAG, "LED indicator OFF, RGB LED 48 set to WHITE (val: %d)", brightness);
}

void app_main(void) {
  /* Matikan seluruh LED sebelum peripheral lain aktif */
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

  /* Start ST7789/ST7735 128x160 SPI TFT LVGL Dashboard Task pinned to Core 0 (Static Task) */
  ESP_LOGI(TAG, "Starting 128x160 SPI TFT LVGL Task (Static)...");
  xTaskCreateStaticPinnedToCore(lcd_task, "lcd_task", LCD_TASK_STACK_SIZE, NULL, 5,
                                s_lcd_task_stack, &s_lcd_task_buffer, 0);

  /* Dispatcher task for telemetry dispatch pinned to Core 0 (Static Task) */
  s_wifi_task_handle = xTaskCreateStaticPinnedToCore(
      WiFi_Server_Task, "WiFi_Server_Task", WIFI_TASK_STACK_SIZE,
      (void *)s_telemetry_queue, 5, s_wifi_task_stack, &s_wifi_task_buffer, 0);

  /* Dedicated HTTP video streamer task pinned to Core 1 (Static Task) */
  s_http_sender_task_handle = xTaskCreateStaticPinnedToCore(
      http_stream_sender_task, "HTTP_Stream_Task", HTTP_STREAM_STACK_SIZE,
      NULL, 4, s_http_stream_task_stack, &s_http_stream_task_buffer, 1);

  /* Telemetry producer task pinned to Core 1 (Static Task) */
  s_test_task_handle = xTaskCreateStaticPinnedToCore(
      test_telemetry_producer_task, "Test_Producer_Task", TEST_PRODUCER_STACK_SIZE,
      (void *)s_telemetry_queue, 3, s_test_producer_task_stack, &s_test_producer_task_buffer, 1);

  ESP_LOGI(TAG, "All system static tasks started successfully.");
}