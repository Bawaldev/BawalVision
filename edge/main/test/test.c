#include "test.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"

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
    strncpy(dummy_data.device_id, "ESP32S3_TEST_NODE", sizeof(dummy_data.device_id) - 1);
    dummy_data.device_id[sizeof(dummy_data.device_id) - 1] = '\0';

    float temp_counter = 25.0f;

    for (;;) {
        time_t now;
        time(&now);

        dummy_data.timestamp = (int64_t)now;
        dummy_data.temperature = temp_counter;
        dummy_data.humidity = 60.5f;
        dummy_data.gas_voc = 12500.0f;
        dummy_data.pressure = 1013.25f;

        ESP_LOGI(TAG, "Mendorong payload dummy ke antrean (Temp: %.2f C)...", dummy_data.temperature);

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