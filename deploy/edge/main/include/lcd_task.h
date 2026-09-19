#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Current operational and self-test state of the device.
 */
typedef enum {
  SYSTEM_STATUS_IDLE =
      0,              /**< Device is ready and waiting for inspection tasks */
  SYSTEM_STATUS_PASS, /**< Last inspection or self-test completed successfully
                       */
  SYSTEM_STATUS_FAIL, /**< System encountered an error or test failure */
} system_status_t;

/**
 * @brief Convert a status enum value into a readable display string.
 *
 * @param status The status enum value to convert.
 * @return const char* String literal ("IDLE", "PASS", "FAIL", or "UNKNOWN").
 */
const char *lcd_status_to_string(system_status_t status);

/**
 * @brief Read internal ESP silicon registers and return the chip model name.
 *
 * Checks hardware chip info (such as ESP32-S3, ESP32-C3) along with silicon
 * revision and active core count. Caches the formatted string in static memory.
 *
 * @return const char* Pointer to a static string (e.g., "ESP32-S3 (r0, 2C)").
 * @note This function is thread-safe and safe to call before the display task
 * starts.
 */
const char *lcd_get_chip_type_string(void);

/**
 * @brief Update the status card and color theme on the LCD screen.
 *
 * Applies amber styling for IDLE, green for PASS, and red for FAIL.
 *
 * @param status New system status to render on the display.
 * @note Thread-safe. Acquires the LVGL port mutex before touching UI objects.
 */
void lcd_update_status(system_status_t status);

/**
 * @brief FreeRTOS task entry point for initializing ST7789 and running the LVGL
 * UI.
 *
 * Configures the SPI bus, sets up the ST7789 panel via esp_lcd, starts the LVGL
 * port task, and renders the initial dashboard.
 *
 * @param pvParameters FreeRTOS task parameter (currently unused, pass NULL).
 */
void lcd_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
