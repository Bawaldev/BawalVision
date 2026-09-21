#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the edge camera subsystem, frame buffers, and FreeRTOS synchronization mutex.
 * 
 * @return esp_err_t ESP_OK on success, or appropriate error code.
 */
esp_err_t camera_task_init(void);

/**
 * @brief Spawns the FreeRTOS Camera Capture Task pinned to the specified core.
 * 
 * @param priority Task priority (e.g. 5).
 * @param core_id CPU Core ID (0 or 1).
 * @return TaskHandle_t Handle of the created FreeRTOS task.
 */
TaskHandle_t camera_task_start(UBaseType_t priority, BaseType_t core_id);

/**
 * @brief FreeRTOS Task function executing continuous camera capture loop.
 * 
 * @param pvParameters Optional task parameters.
 */
void Camera_Capture_Task(void *pvParameters);

/**
 * @brief Acquire the camera frame buffer mutex with a specified timeout.
 * 
 * @param timeout_ticks Maximum ticks to wait for mutex acquisition.
 * @return true If mutex was successfully acquired.
 * @return false If timeout occurred.
 */
bool camera_lock_frame(TickType_t timeout_ticks);

/**
 * @brief Release the camera frame buffer mutex.
 */
void camera_unlock_frame(void);

/**
 * @brief Get a read-only pointer to the latest shared camera frame buffer.
 * Note: Must be called while holding camera_lock_frame() to guarantee consistency!
 * 
 * @return const camera_frame_buffer_t* Pointer to shared frame container.
 */
const camera_frame_buffer_t* camera_get_shared_frame(void);

/**
 * @brief Safely copy the latest frame into an external buffer under mutex lock.
 * 
 * @param out_frame Destination frame container.
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT or ESP_ERR_NO_MEM on failure.
 */
esp_err_t camera_copy_latest_frame(camera_frame_buffer_t *out_frame);

/**
 * @brief Safely encodes the latest frame buffer into a Base64 string under mutex lock.
 * 
 * @param out_b64 Pointer to char* where allocated string will be placed (caller must free()).
 * @param out_len Pointer to size_t where length will be placed.
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT / ESP_ERR_NO_MEM on failure.
 */
esp_err_t camera_get_latest_frame_base64(char **out_b64, size_t *out_len);

/**
 * @brief Serialize camera frame metadata and optional base64 image data to a JSON string.
 * 
 * @param frame Pointer to the frame container to serialize.
 * @param out_json_str Pointer to char* where allocated JSON string will be returned (caller must free()).
 * @param include_base64 If true, encodes raw image bytes as a base64 "image_base64" field in JSON.
 * @return esp_err_t ESP_OK on success, or ESP_ERR_NO_MEM / ESP_FAIL.
 */
esp_err_t camera_frame_to_json(const camera_frame_buffer_t *frame, char **out_json_str, bool include_base64);

#ifdef __cplusplus
}
#endif
