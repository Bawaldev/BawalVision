#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
static const char *TAG = "MAIN";

void app_main(void) {
    // Recommended ESP-IDF logging (includes timestamp and log tag)
    ESP_LOGI(TAG, "Edge Node Initialized successfully!");

    // Standard C printf (requires newline or fflush to flush the output buffer)
    printf("Hello from ESP32-S3 serial output!\n");
}