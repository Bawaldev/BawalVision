#pragma once

#include "esp_chip_info.h"
#include "sdkconfig.h"

/**
 * @file pins.h
 * @brief Hardware pin mapping and peripheral assignments for BawalVision edge
 * node.
 */

/* =========================================================================
 * ST7789 SPI LCD Display Wiring
 * ========================================================================= */

/** @brief SPI controller host used by the display panel (SPI2_HOST = FSPI). */
#define LCD_HOST SPI2_HOST

/** @brief Master Out Slave In (SDA/MOSI) data line to LCD. */
#define LCD_PIN_MOSI 11

/** @brief SPI clock line (SCL/SCK). */
#define LCD_PIN_SCK 12

/** @brief Data / Command control pin (high for data, low for command). */
#define LCD_PIN_DC 21

/** @brief Panel reset pin (-1 if tied to ESP EN/RST). */
#define LCD_PIN_RST -1

/** @brief SPI chip select pin (-1 if panel CS is hardwired to GND). */
#define LCD_PIN_CS -1

/** @brief Backlight control pin (-1 if hardwired to 3.3V or unconnected). */
#define LCD_PIN_BCKL -1

/** @brief Horizontal resolution in pixels. */
#define LCD_H_RES 240

/** @brief Vertical resolution in pixels. */
#define LCD_V_RES 240

/** @brief SPI pixel clock frequency in Hertz (40 MHz default for ST7789). */
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

/* =========================================================================
 * Legacy / Secondary Display Pinout (ESP32-S3)
 * ========================================================================= */
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CHIP_ESP32S3)

/* --- Nextion Display / Secondary Bus --- */
#define GPIO_SCK 12
#define GPIO_MISO 11
#define GPIO_SDA 8
#define GPIO_SCL 9

#endif