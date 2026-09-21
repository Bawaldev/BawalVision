#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_ID_MAX_LEN 64
#define DEVICE_NAME_MAX_CHAR 100
#define FW_VERSION_MAX_LEN 54

/**
 * @brief Supported pixel formats for camera frame buffer.
 */
typedef enum {
    FRAME_FORMAT_JPEG = 0,
    FRAME_FORMAT_RGB565 = 1,
    FRAME_FORMAT_RGB888 = 2,
    FRAME_FORMAT_GRAYSCALE = 3,
    FRAME_FORMAT_RAW = 4
} frame_format_t;

/**
 * @brief Packed camera frame metadata structure (zero padding for byte-level efficiency
 * and wire compatibility across C, C++ OpenCV, and Go backend).
 */
typedef struct __attribute__((__packed__)) {
    uint32_t frame_id;         /**< Monotonic sequence identifier of the captured frame */
    uint32_t width;            /**< Frame width in pixels */
    uint32_t height;           /**< Frame height in pixels */
    uint32_t format;           /**< Pixel format enum (frame_format_t) */
    uint32_t buffer_len;       /**< Exact byte length of the encoded image data */
    int64_t  timestamp_ms;     /**< Millisecond epoch or uptime timestamp of capture */
} camera_frame_header_t;

/**
 * @brief Environmental, system health, and identification telemetry packet.
 * Packed attribute removes compiler struct padding alignment bytes.
 */
typedef struct __attribute__((__packed__)) {
    char device_id[DEVICE_ID_MAX_LEN];         /**< Unique device ID string (<CHIP>_<MAC>) */
    char firmware_version[FW_VERSION_MAX_LEN]; /**< Application version string */
    int64_t timestamp;                         /**< Millisecond epoch or uptime timestamp */
    float temperature;                         /**< Ambient temperature in Celsius */
    float humidity;                            /**< Relative humidity percentage */
    float gas_voc;                             /**< VOC gas sensor index or resistance */
    float pressure;                            /**< Barometric pressure in hPa */
    uint8_t current_cpu_load;                  /**< CPU load percentage (0 - 100) */
    char device_name[DEVICE_NAME_MAX_CHAR];    /**< Project/Node identification name */
    uint8_t device_core;                       /**< Core ID running the telemetry task */
} telemetry_data_t;

/**
 * @brief Thread-safe in-memory camera frame container.
 */
typedef struct {
    camera_frame_header_t header; /**< Zero-padded frame metadata */
    uint8_t *data;                /**< Pointer to image buffer (PSRAM / Heap) */
    size_t capacity;              /**< Allocated buffer capacity in bytes */
} camera_frame_buffer_t;

/**
 * @brief Combined payload packaging telemetry metrics alongside captured camera frame.
 */
typedef struct __attribute__((__packed__)) {
    telemetry_data_t telemetry;       /**< Embedded sensor telemetry packet */
    camera_frame_header_t frame_info; /**< Zero-padded camera frame header */
    uint8_t *image_buf;               /**< Pointer to image buffer (e.g. JPEG in PSRAM) */
    size_t image_len;                 /**< Size of captured image in bytes */
} inspection_payload_t;

#ifdef __cplusplus
}
#endif