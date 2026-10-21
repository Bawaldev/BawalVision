#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data.h"
#include "esp_err.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_FRAME_WIDTH  640
#define DEFAULT_FRAME_HEIGHT 480
#define INITIAL_BUF_CAPACITY (256 * 1024) /* 256 KB PSRAM buffer */
#define DEVICE_ID            "ESP32-S3-BENCH-01"
/* 15 FPS keeps the ~7 KB JPEG stream within weak-AP airtime budgets and
   leaves room for telemetry/control traffic. The vision service is the
   downstream bottleneck anyway. */
#define STREAM_FPS           15

/**
 * 
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
 * @brief Worker task for UVC video stream ingestion.
 * 
 * @param pvParameters Optional FreeRTOS task arguments.
 */
void Camera_Capture_Task(void *pvParameters);

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

/**
 * @brief Returns whether the camera is currently actively streaming frames.
 * 
 * @return true if streaming, false otherwise.
 */
bool is_camera_streaming(void);

/**
 * @brief Returns the scanned camera sensor model name (e.g. "OV5640", "OV3660", "OV2640").
 * 
 * @return const char* Camera sensor model string.
 */
const char *camera_get_sensor_name(void);

#ifdef __cplusplus
}
#endif