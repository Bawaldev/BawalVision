#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_FRAME_WIDTH  640
#define DEFAULT_FRAME_HEIGHT 480
#define STREAM_FPS           14

/**
 * @brief Initialize the OV5640-compatible camera in JPEG VGA mode.
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

#ifdef __cplusplus
}
#endif
