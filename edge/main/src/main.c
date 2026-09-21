#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>

#include "data.h"
#include "lcd_task.h"
#include "camera_task.h"
#include "test.h"
#include "wifi.h"

static const char *TAG = "MAIN";

static QueueHandle_t s_telemetry_queue = NULL;
static TaskHandle_t s_wifi_task_handle = NULL;
static TaskHandle_t s_test_task_handle = NULL;
static TaskHandle_t s_lcd_task_handle = NULL;
static TaskHandle_t s_camera_task_handle = NULL;

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  s_telemetry_queue = xQueueCreate(5, sizeof(telemetry_data_t));
  if (s_telemetry_queue == NULL) {
    ESP_LOGE(TAG, "Gagal mengalokasikan FreeRTOS Queue");
    return;
  }

  // Initialize and spawn camera FreeRTOS task with mutex synchronization
  ESP_ERROR_CHECK(camera_task_init());
  s_camera_task_handle = camera_task_start(4, 1);

  wifi_init_sta();
  xTaskCreatePinnedToCore(WiFi_Server_Task, "WiFi_Server_Task", 4096,
                          (void *)s_telemetry_queue, 5, &s_wifi_task_handle, 0);

  xTaskCreatePinnedToCore(test_telemetry_producer_task, "Test_Producer_Task",
                          3072, (void *)s_telemetry_queue, 4,
                          &s_test_task_handle, 1);

  xTaskCreatePinnedToCore(lcd_task, "LCD_Task", 6144, NULL, 3,
                          &s_lcd_task_handle, 1);
}