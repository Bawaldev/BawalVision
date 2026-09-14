#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "data.h"
#include "wifi.h"
#include "test.h"

static const char *TAG = "MAIN";

static QueueHandle_t s_telemetry_queue = NULL;
static TaskHandle_t s_wifi_task_handle = NULL;
static TaskHandle_t s_test_task_handle = NULL;

void app_main(void) {
    // 1. Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Init Network Stack & Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Init Queue Telemetri
    s_telemetry_queue = xQueueCreate(5, sizeof(telemetry_data_t));
    if (s_telemetry_queue == NULL) {
        ESP_LOGE(TAG, "Gagal mengalokasikan FreeRTOS Queue");
        return;
    }

    // 4. Connect Wi-Fi STA bawaan kamu
    wifi_init_sta();

    // 5. Run Tasks
    xTaskCreatePinnedToCore(
        WiFi_Server_Task,
        "WiFi_Server_Task",
        4096,
        (void *)s_telemetry_queue,
        5,
        &s_wifi_task_handle,
        0
    );

    xTaskCreatePinnedToCore(
        test_telemetry_producer_task,
        "Test_Producer_Task",
        3072,
        (void *)s_telemetry_queue,
        4,
        &s_test_task_handle,
        1
    );
}