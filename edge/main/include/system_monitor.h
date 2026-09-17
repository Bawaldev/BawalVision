#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "data.h"

uint8_t get_cpu_usage_percentage(void);


void get_internal_device_id(char *out_id, size_t max_len);
void init_device_metadata(telemetry_data_t *data);
uint8_t get_system_cpu_load(void);