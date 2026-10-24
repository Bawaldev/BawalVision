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
<<<<<<< HEAD
#define INITIAL_BUF_CAPACITY (256 * 1024) /* 256 KB PSRAM buffer */
#define DEVICE_ID            "ESP32-S3-BENCH-01"
/* 15 FPS keeps the ~7 KB JPEG stream within weak-AP airtime budgets and
   leaves room for telemetry/control traffic. The vision service is the
   downstream bottleneck anyway. */
#define STREAM_FPS           15
=======
#define DEFAULT_FPS          30.0f
#define STREAM_FPS           20
#define INITIAL_BUF_CAPACITY (256 * 1024)

/** Double-buffered frame slot: metadata header plus a heap/PSRAM pixel buffer. */
typedef struct __attribute__((aligned(8))) {
  camera_frame_header_t header;
  uint8_t *data;
} frame_slot_t;
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444

/**
 * @brief Initialize the OV5640-compatible camera in crisp JPEG VGA mode (640x480).
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

/** Copy the current latest frame without consuming its availability flag. */
esp_err_t camera_copy_current_frame(camera_frame_buffer_t *out_frame);

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

<<<<<<< HEAD
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

=======
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
#ifdef __cplusplus
}
#endif
