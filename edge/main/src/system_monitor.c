#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "data.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t s_temp_sensor = NULL;
static bool s_temp_sensor_enabled = false;
static float s_last_valid_temp = 0.0f;

float get_esp32_cpu_temperature(void) {
    if (s_temp_sensor == NULL) {
        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t err = temperature_sensor_install(&temp_sensor_config, &s_temp_sensor);
        if (err == ESP_OK && s_temp_sensor != NULL) {
            if (temperature_sensor_enable(s_temp_sensor) == ESP_OK) {
                s_temp_sensor_enabled = true;
            } else {
                temperature_sensor_uninstall(s_temp_sensor);
                s_temp_sensor = NULL;
                s_temp_sensor_enabled = false;
            }
        } else {
            s_temp_sensor = NULL;
            s_temp_sensor_enabled = false;
        }
    }

    if (s_temp_sensor != NULL && s_temp_sensor_enabled) {
        float tsens_out = 0.0f;
        if (temperature_sensor_get_celsius(s_temp_sensor, &tsens_out) == ESP_OK) {
            s_last_valid_temp = tsens_out;
            return tsens_out;
        }
    }
    return s_last_valid_temp;
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static uint32_t s_prev_total_runtime = 0;
static uint32_t s_prev_idle_runtime = 0;
static uint8_t s_last_cpu_percent = 0;
#endif

uint8_t get_cpu_usage_percentage(void) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    UBaseType_t array_size = task_count + 8;
    TaskStatus_t stack_tasks[36];
    TaskStatus_t *task_array = (array_size <= 36) ? stack_tasks : (TaskStatus_t *)pvPortMalloc(array_size * sizeof(TaskStatus_t));
    if (task_array == NULL) {
        return s_last_cpu_percent;
    }

    uint32_t current_total_time = 0;
    UBaseType_t actual_count = uxTaskGetSystemState(task_array, (task_array == stack_tasks) ? 36 : array_size, &current_total_time);
    if (actual_count == 0) {
        if (task_array != stack_tasks) vPortFree(task_array);
        return s_last_cpu_percent;
    }

    uint32_t current_idle_time = 0;
    for (UBaseType_t i = 0; i < actual_count; i++) {
        // In ESP-IDF FreeRTOS SMP, idle tasks are named "IDLE0" (core 0) and "IDLE1" (core 1)
        if (strncmp(task_array[i].pcTaskName, "IDLE", 4) == 0) {
            current_idle_time += task_array[i].ulRunTimeCounter;
        }
    }

    if (task_array != stack_tasks) {
        vPortFree(task_array);
    }

    // First call initialization: take a short baseline sample once
    if (s_prev_total_runtime == 0) {
        s_prev_total_runtime = current_total_time;
        s_prev_idle_runtime = current_idle_time;
        vTaskDelay(pdMS_TO_TICKS(50));
        return get_cpu_usage_percentage();
    }

    uint32_t delta_total = current_total_time - s_prev_total_runtime;
    // Debounce rapid consecutive reads (< 25ms) to preserve accuracy
    if (delta_total < 25000) {
        return s_last_cpu_percent;
    }

    uint32_t delta_idle = current_idle_time - s_prev_idle_runtime;
    s_prev_total_runtime = current_total_time;
    s_prev_idle_runtime = current_idle_time;

    uint32_t num_cores = CONFIG_FREERTOS_NUMBER_OF_CORES;
    if (num_cores == 0) num_cores = 1;

    // Total execution capacity across all active physical cores in the sample interval
    uint32_t total_capacity = delta_total * num_cores;
    if (total_capacity == 0) return 0;

    uint32_t idle_clamped = (delta_idle > total_capacity) ? total_capacity : delta_idle;
    uint32_t cpu_busy = total_capacity - idle_clamped;
    uint32_t cpu_percentage = (cpu_busy * 100) / total_capacity;

    if (cpu_percentage > 100) cpu_percentage = 100;
    s_last_cpu_percent = (uint8_t)cpu_percentage;
    return s_last_cpu_percent;
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
    return get_cpu_usage_percentage();
}