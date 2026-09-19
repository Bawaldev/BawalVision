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
 * @brief Environmental, system health, and identification telemetry packet.
 */
typedef struct __attribute__((__packed__)) {
  char device_id[DEVICE_ID_MAX_LEN]; /**< Unique device ID string (<CHIP>_<MAC>)
                                      */
  char firmware_version[FW_VERSION_MAX_LEN]; /**< Application version string */
  int64_t timestamp;        /**< Millisecond epoch or uptime timestamp */
  float temperature;        /**< Ambient temperature in Celsius */
  float humidity;           /**< Relative humidity percentage */
  float gas_voc;            /**< VOC gas sensor index or resistance */
  float pressure;           /**< Barometric pressure in hPa */
  uint8_t current_cpu_load; /**< CPU load percentage (0 - 100) */
  char device_name[DEVICE_NAME_MAX_CHAR]; /**< Project/Node identification name
                                           */
  uint8_t device_core; /**< Core ID running the telemetry task */
} telemetry_data_t;

/**
 * @brief Combined payload packaging telemetry metrics alongside captured camera
 * frame.
 */
typedef struct __attribute__((__packed__)) {
  telemetry_data_t telemetry; /**< Embedded sensor telemetry packet */
  uint8_t *image_buf; /**< Pointer to image buffer (e.g. JPEG in PSRAM) */
  size_t image_len;   /**< Size of captured image in bytes */
} inspection_payload_t;

#ifdef __cplusplus
}
#endif