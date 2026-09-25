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

static QueueHandle_t s_telemetry_queue = NULL;
static TaskHandle_t s_wifi_task_handle = NULL;
static TaskHandle_t s_test_task_handle = NULL;
static TaskHandle_t s_camera_task_handle = NULL;
static TaskHandle_t s_http_sender_task_handle = NULL;
// s_lcd_task_handle dihapus sementara untuk menghilangkan compiler warning

void app_main(void) {
  // 1. Inisialisasi Storage NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 2. Inisialisasi Network Stack & Event Loop Default
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // 3. Buat Queue Telemetry Data
  s_telemetry_queue = xQueueCreate(5, sizeof(telemetry_data_t));
  if (s_telemetry_queue == NULL) {
    ESP_LOGE(TAG, "Gagal mengalokasikan FreeRTOS Queue");
    return;
  }

  // 4. Inisialisasi Driver UVC & USB Host (Dilakukan sebelum Wi-Fi agar stack USB siap)
  ESP_LOGI(TAG, "Initializing UVC Camera Driver...");
  ret = camera_task_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Gagal menginisialisasi UVC Driver: %s", esp_err_to_name(ret));
  } else {
    // Jalankan Camera Capture Task di Core 1 (Priority 5 agar tidak terganggu HTTP task)
    s_camera_task_handle = camera_task_start(5, 1);
  }

  // 5. Inisialisasi Wi-Fi Station & Start Dispatcher Task
  ESP_LOGI(TAG, "Connecting to Wi-Fi Network...");
  wifi_init_sta();
  
  // WiFi Server Task menangani pengiriman Telemetry di Core 0
  xTaskCreatePinnedToCore(WiFi_Server_Task, "WiFi_Server_Task", 4096,
                          (void *)s_telemetry_queue, 5, &s_wifi_task_handle, 0);

  // 6. Jalankan Dedicated HTTP Video Streamer Task di Core 1 (Priority 4)
  xTaskCreatePinnedToCore(http_stream_sender_task, "HTTP_Stream_Task", 4096,
                          NULL, 4, &s_http_sender_task_handle, 1);

  // 7. Jalankan Test Telemetry Producer Task di Core 1 (Priority 3)
  xTaskCreatePinnedToCore(test_telemetry_producer_task, "Test_Producer_Task",
                          3072, (void *)s_telemetry_queue, 3,
                          &s_test_task_handle, 1);

  ESP_LOGI(TAG, "All system tasks started successfully.");
}