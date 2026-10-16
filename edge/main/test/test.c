#include "test.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "system_monitor.h"
#include "data.h"

static const char *TAG = "TELEMETRY_PRODUCER";

void test_telemetry_producer_task(void *pvParameters) {
    QueueHandle_t data_queue = (QueueHandle_t)pvParameters;
    if (data_queue == NULL) {
        ESP_LOGE(TAG, "Queue handle is NULL, terminating telemetry producer task.");
        vTaskDelete(NULL);
        return;
    }

    telemetry_data_t live_data;
    init_device_metadata(&live_data);

    for (;;) {
        time_t now;
        time(&now);
        live_data.timestamp = (int64_t)now;
        live_data.temperature = get_esp32_cpu_temperature();
        live_data.current_cpu_load = get_cpu_usage_percentage();

        ESP_LOGI(TAG, "Enqueuing hardware telemetry [ID: %s, Temp: %.2f C, CPU: %d%%]...", 
                 live_data.device_id, live_data.temperature, live_data.current_cpu_load);

        /* Emit structured telemetry frame over USB Serial/UART for direct server detection */
        printf("BAWAL_TELEMETRY:{\"device_id\":\"%s\",\"firmware_version\":\"%s\",\"temperature\":%.2f,\"cpu_load\":%u}\n",
               live_data.device_id, live_data.firmware_version, live_data.temperature, live_data.current_cpu_load);
        fflush(stdout);

        if (xQueueSend(data_queue, &live_data, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Telemetry queue full, dropping telemetry packet.");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}