#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Calculate actual CPU core utilization over a 100ms sample window.
 *
 * Measures runtime counters across tasks compared against the FreeRTOS idle tasks.
 * Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS to be enabled.
 *
 * @return uint8_t Estimated CPU usage percentage (0 - 100).
 */
uint8_t get_cpu_usage_percentage(void);

/**
 * @brief Retrieve the hardware chip family name string from internal registers.
 *
 * @return const char* Chip model name (e.g. "ESP32S3", "ESP32C3", or "ESP32_UNKNOWN").
 */
const char *get_esp_chip_model_name(void);

/**
 * @brief Generate a unique hardware identifier string using chip model and Wi-Fi MAC.
 *
 * Format: `<MODEL>_<MAC_HEX>` (for example: `ESP32S3_A1B2C3D4E5F6`).
 *
 * @param[out] out_id Destination character buffer.
 * @param[in]  max_len Maximum buffer capacity in bytes.
 */
void get_internal_device_id(char *out_id, size_t max_len);

/**
 * @brief Populate default device telemetry metadata (ID, firmware version, node name).
 *
 * Reads project description headers and active CPU core ID.
 *
 * @param[out] data Pointer to telemetry data structure to initialize.
 */
void init_device_metadata(telemetry_data_t *data);

/**
 * @brief Estimate approximate system memory pressure as a percentage.
 *
 * Calculates the delta between current free heap and historic lowest heap watermark.
 *
 * @return uint8_t Heap memory utilization percentage (0 - 100).
 */
uint8_t get_system_cpu_load(void);

#ifdef __cplusplus
}
#endif