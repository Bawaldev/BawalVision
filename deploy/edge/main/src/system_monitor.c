#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "data.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_chip_info.h"

uint8_t get_cpu_usage_percentage(void) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t start_array[16], end_array[16];
    UBaseType_t start_count, end_count;
    uint32_t start_run_time, end_run_time;

    start_count = uxTaskGetSystemState(start_array, 16, &start_run_time);
    vTaskDelay(pdMS_TO_TICKS(100));
    end_count = uxTaskGetSystemState(end_array, 16, &end_run_time);

    uint32_t total_elapsed = end_run_time - start_run_time;
    if (total_elapsed == 0) return 0;

    uint32_t idle_time = 0;
    for (UBaseType_t i = 0; i < end_count; i++) {
        if (strncmp(end_array[i].pcTaskName, "IDLE", 4) == 0) {
            for (UBaseType_t j = 0; j < start_count; j++) {
                if (end_array[i].xHandle == start_array[j].xHandle) {
                    idle_time += (end_array[i].ulRunTimeCounter - start_array[j].ulRunTimeCounter);
                    break;
                }
            }
        }
    }

    uint32_t cpu_busy = total_elapsed - (idle_time / CONFIG_FREERTOS_NUMBER_OF_CORES);
    uint32_t cpu_percentage = (cpu_busy * 100) / total_elapsed;

    if (cpu_percentage > 100) cpu_percentage = 100;
    return (uint8_t)cpu_percentage;
#else
    return 0; 
#endif
}


const char* get_esp_chip_model_name(void) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    switch (chip_info.model) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32S2";
        case CHIP_ESP32S3: return "ESP32S3";
        case CHIP_ESP32C3: return "ESP32C3";
        case CHIP_ESP32C2: return "ESP32C2";
        case CHIP_ESP32C6: return "ESP32C6";
        case CHIP_ESP32H2: return "ESP32H2";
        default:           return "ESP32_UNKNOWN";
    }
}

void get_internal_device_id(char *out_id, size_t max_len) {
    if (out_id == NULL || max_len == 0) return;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    const char *model_name = get_esp_chip_model_name();
    snprintf(out_id, max_len, "%s_%02X%02X%02X%02X%02X%02X", 
             model_name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
void init_device_metadata(telemetry_data_t *data) {
    if (data == NULL) return;

    memset(data, 0, sizeof(telemetry_data_t));
    get_internal_device_id(data->device_id, sizeof(data->device_id));
    const esp_app_desc_t *app_desc = esp_app_get_description();
    snprintf(data->firmware_version, sizeof(data->firmware_version), "%s", app_desc->version);
    snprintf(data->device_name, sizeof(data->device_name), "%s_NODE", app_desc->project_name);
    data->device_core = (uint8_t)xPortGetCoreID();
}

uint8_t get_system_cpu_load(void) {
    uint32_t total_heap = esp_get_free_heap_size() + esp_get_minimum_free_heap_size();
    uint32_t free_heap = esp_get_free_heap_size();
    
    if (total_heap == 0) return 0;
    
    uint8_t usage = (uint8_t)(((total_heap - free_heap) * 100) / total_heap);
    return usage;
}