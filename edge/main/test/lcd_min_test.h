#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FreeRTOS task entry point for bare-metal minimum ST7789 LCD hardware test.
 * 
 * Bypasses LVGL entirely to test raw SPI communication, panel controller initialization,
 * and RGB color rendering on 240x240 displays.
 * 
 * @param pvParameters FreeRTOS task parameter (unused, pass NULL).
 */
void lcd_min_test_task(void *pvParameters);

/**
 * @brief Direct standalone function to run a 1-shot minimum LCD hardware test.
 * Can be called synchronously directly from app_main().
 */
esp_err_t lcd_min_test_run(void);

#ifdef __cplusplus
}
#endif

