#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Current operational and inspection state of the device.
 */
typedef enum {
  SYSTEM_STATUS_IDLE = 0, /**< Device is ready and waiting for inspection tasks */
  SYSTEM_STATUS_PASS,     /**< Food tray inspection PASSED */
  SYSTEM_STATUS_FAIL,     /**< Food tray inspection REJECTED or system error */
  SYSTEM_STATUS_REVIEW,   /**< Food tray requires manual review */
} system_status_t;

/**
 * @brief Convert a status enum value into a readable display string.
 */
const char *lcd_status_to_string(system_status_t status);

/**
 * @brief Read internal ESP silicon registers and return the chip model name.
 */
const char *lcd_get_chip_type_string(void);

/**
 * @brief Update the system inspection status card and color theme on the LCD.
 *
 * @param status New system status (IDLE = Amber, PASS = Green, FAIL = Red, REVIEW = Orange).
 */
void lcd_update_status(system_status_t status);

/**
 * @brief Update the top HTTP communication status banner on the 128x160 TFT.
 *
 * @param status_str String to display (e.g. "POST TELEMETRY", "HTTP 200 OK", "HTTP TIMEOUT").
 * @param is_error Set true for red error indicator, false for active/success color.
 */
void lcd_update_http_status(const char *status_str, bool is_error);

/**
 * @brief Update the Wi-Fi connection indicator and IP address banner on the LCD.
 *
 * @param connected True if Wi-Fi has acquired an IP address.
 * @param ip_address Assigned IP string (e.g. "192.168.1.50") or NULL.
 */
void lcd_update_wifi_status(bool connected, const char *ip_address);

/**
 * @brief Update the real-time sensor strip on the LCD (ESP32 Temp, CPU Load).
 *
 * @param temperature Current ESP32-S3 hardware temperature in °C.
 * @param cpu_load Current ESP32 CPU load percentage (0-100%).
 */
void lcd_update_telemetry_display(float temperature, uint8_t cpu_load);

/**
 * @brief Update active connection status (ONLINE/OFFLINE) and communication latency.
 *
 * @param connected True if device is actively linked to network and backend.
 * @param latency_ms Round-trip communication latency in milliseconds (0 for placeholder).
 */
void lcd_update_connection_status(bool connected, uint32_t latency_ms);

/**
 * @brief Update industrial conveyor line speed telemetry in meters per second (m/s).
 *
 * @param speed_mps Speed in meters per second (e.g. 0.35f).
 */
void lcd_update_line_speed(float speed_mps);

/**
 * @brief FreeRTOS task entry point for initializing the 128x160 SPI TFT and LVGL UI.
 *
 * @param pvParameters FreeRTOS task parameter (pass NULL).
 */
void lcd_task(void *pvParameters);

/**
 * @brief Lightweight background task for updating live connection heartbeat and metrics on LCD.
 *
 * @param pvParameters FreeRTOS task parameter (pass NULL).
 */
void lcd_monitor_heartbeat_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
