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

void app_main(void) {
  /* Set USB Host and UVC log verbosity to DEBUG for physical signal and protocol tracing */
  esp_log_level_set("usb_host", ESP_LOG_DEBUG);
  esp_log_level_set("uvc_camera", ESP_LOG_DEBUG);
  esp_log_level_set("usbh_uvc", ESP_LOG_DEBUG);

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

  /* Create telemetry data queue */
  s_telemetry_queue = xQueueCreate(5, sizeof(telemetry_data_t));
  if (s_telemetry_queue == NULL) {
    ESP_LOGE(TAG, "Failed to allocate FreeRTOS Queue");
    return;
  }

  /* Initialize UVC driver and USB Host stack */
  ESP_LOGI(TAG, "Initializing UVC Camera Driver...");
  ret = camera_task_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize UVC Driver: %s", esp_err_to_name(ret));
  } else {
    s_camera_task_handle = camera_task_start(5, 1);
  }

  /* Connect to Wi-Fi station */
  ESP_LOGI(TAG, "Connecting to Wi-Fi Network...");
  wifi_init_sta();

  /* Dispatcher task for telemetry dispatch pinned to Core 0 */
  xTaskCreatePinnedToCore(WiFi_Server_Task, "WiFi_Server_Task", 4096,
                          (void *)s_telemetry_queue, 5, &s_wifi_task_handle, 0);

  /* Dedicated HTTP video streamer task pinned to Core 1 */
  xTaskCreatePinnedToCore(http_stream_sender_task, "HTTP_Stream_Task", 4096,
                          NULL, 4, &s_http_sender_task_handle, 1);

  /* Telemetry producer task pinned to Core 1 */
  xTaskCreatePinnedToCore(test_telemetry_producer_task, "Test_Producer_Task",
                          3072, (void *)s_telemetry_queue, 3,
                          &s_test_task_handle, 1);

  ESP_LOGI(TAG, "All system tasks started successfully.");
}