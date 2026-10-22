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
#define STREAM_FPS           12
#define INITIAL_BUF_CAPACITY (256 * 1024)

/**
 * @brief Initialize the OV5640-compatible camera in JPEG QVGA mode.
 *
 * @return esp_err_t ESP_OK on success, or the esp_camera error code.
 */
esp_err_t camera_task_init(void);

/**
 * @brief Return true when the camera driver initialized successfully.
 */
bool camera_task_is_ready(void);

/**
 * @brief Return the detected camera sensor model name (for example "OV5640").
 */
const char *camera_get_sensor_name(void);

/**
 * @brief Copy latest frame from double-buffering slot to destination.
 */
esp_err_t camera_copy_latest_frame(camera_frame_buffer_t *out_frame);

/**
 * @brief Return whether camera is streaming.
 */
bool is_camera_streaming(void);

/**
 * @brief Start camera capture static task.
 */
TaskHandle_t camera_task_start(UBaseType_t priority, BaseType_t core_id);

/**
 * @brief Camera capture task function.
 */
void Camera_Capture_Task(void *pvParameters);

#ifdef __cplusplus
}
#endif
