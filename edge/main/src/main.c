#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

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

void app_main(void) {
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