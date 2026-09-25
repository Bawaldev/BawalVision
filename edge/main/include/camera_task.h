#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif
#define DEFAULT_FRAME_WIDTH  640
#define DEFAULT_FRAME_HEIGHT 480
#define DEFAULT_FPS          15.0f
#define INITIAL_BUF_CAPACITY (256 * 1024) // 256 KB PSRAM buffer
#define DEVICE_ID            "ESP32-S3-BENCH-01"
#define STREAM_FPS           15

/**
 * @brief Initialize UVC camera hardware, PSRAM buffers, and FreeRTOS synchronization mutex.
 * 
 * @return esp_err_t ESP_OK on success, or appropriate error code.
 */
esp_err_t camera_task_init(void);

/**
 * @brief Spawns the FreeRTOS Camera Capture Task pinned to the specified core.
 * 
 * @param priority Task priority (e.g., 5).
 * @param core_id CPU Core ID (0 or 1).
 * @return TaskHandle_t Handle of the created FreeRTOS task.
 */
TaskHandle_t camera_task_start(UBaseType_t priority, BaseType_t core_id);

/**
 * @brief FreeRTOS task to continuously stream JPEG frames to the backend REST API via HTTP POST.
 * 
 * @param pvParameters Optional task parameters.
 */
void http_stream_sender_task(void *pvParameters);

/**
 * @brief Thread-safely copies the latest JPEG frame from the shared camera buffer.
 * 
 * @param out_frame Destination frame container buffer.
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT, or ESP_ERR_NO_MEM.
 */
esp_err_t camera_copy_latest_frame(camera_frame_buffer_t *out_frame);

#ifdef __cplusplus
}
#endif