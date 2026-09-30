#include "test.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "system_monitor.h"
#include "data.h"

static const char *TAG = "TEST_PRODUCER";

void test_telemetry_producer_task(void *pvParameters) {
    QueueHandle_t data_queue = (QueueHandle_t)pvParameters;
    if (data_queue == NULL) {
        ESP_LOGE(TAG, "Queue handle bernilai NULL, task dihentikan.");
        vTaskDelete(NULL);
        return;
    }

    telemetry_data_t dummy_data;
    init_device_metadata(&dummy_data);

    float temp_counter = 25.0f;

    for (;;) {
        time_t now;
        time(&now);
        dummy_data.timestamp = (int64_t)now;
        dummy_data.temperature = temp_counter;
        dummy_data.current_cpu_load = get_cpu_usage_percentage();

        ESP_LOGI(TAG, "Mendorong payload ke antrean [ID: %s, Temp: %.2f C, CPU: %d%%]...", 
                 dummy_data.device_id, dummy_data.temperature, dummy_data.current_cpu_load);

        if (xQueueSend(data_queue, &dummy_data, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Antrean penuh, payload dilewati.");
        }

        temp_counter += 0.5f;
        if (temp_counter > 35.0f) {
            temp_counter = 25.0f;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}