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
 * @brief Calculate actual CPU core utilization across active physical cores.
 *
 * Measures runtime counters across tasks compared against FreeRTOS idle tasks
 * ("IDLE0" and "IDLE1") using non-blocking snapshot deltas without artificial delays.
 * Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS to be enabled.
 *
 * @return uint8_t Estimated CPU usage percentage (0 - 100).
 */
uint8_t get_cpu_usage_percentage(void);

/**
 * @brief Per-core CPU utilization from FreeRTOS idle-task runtime counters.
 *
 * @param[out] core0 Core 0 (Wi-Fi/protocol) usage percentage, 0 - 100.
 * @param[out] core1 Core 1 (application/camera) usage percentage, 0 - 100.
 */
void get_cpu_core_loads(uint8_t *core0, uint8_t *core1);

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
 * @brief Retrieve actual system CPU load percentage.
 *
 * Alias to get_cpu_usage_percentage().
 *
 * @return uint8_t CPU utilization percentage (0 - 100).
 */
uint8_t get_system_cpu_load(void);

/**
 * @brief Read the real ESP32-S3 internal temperature sensor in degrees Celsius.
 *
 * Uses the ESP-IDF temperature_sensor driver (range -10 C to 80 C).
 *
 * @return float Measured temperature in degrees Celsius.
 */
float get_esp32_cpu_temperature(void);

#ifdef __cplusplus
}
#endif