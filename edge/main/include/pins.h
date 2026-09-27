#pragma once

#include "esp_chip_info.h"
#include "sdkconfig.h"

/**
 * @file pins.h
 * @brief Hardware pin mapping and peripheral assignments for BawalVision edge node.
 * Architecture: ESP32-S3-DevKitC-1 (BawalVision v1 schematic)
 */

/* =========================================================================
 * ST7789 SPI LCD Display Wiring (TFT 1.3")
 * ========================================================================= */

/** @brief SPI controller host used by the display panel (SPI2_HOST = FSPI). */
#define LCD_HOST                SPI2_HOST

/** @brief Master Out Slave In (SDA/MOSI) data line to LCD (GPIO11). */
#define LCD_PIN_MOSI            11

/** @brief SPI clock line (SCL/SCK) (GPIO13). */
#define LCD_PIN_SCK             13

/** @brief Data / Command control pin (GPIO21). */
#define LCD_PIN_DC              21

/** @brief Panel reset pin (-1: tied to ESP EN/RST pin in schematic). */
#define LCD_PIN_RST             -1

/** @brief SPI chip select pin (-1: CS-less panel module). */
#define LCD_PIN_CS              -1

/** @brief Backlight control pin (-1: unconnected / NC in schematic). */
#define LCD_PIN_BCKL            -1

/** @brief Horizontal resolution in pixels. */
#define LCD_H_RES               240

/** @brief Vertical resolution in pixels. */
#define LCD_V_RES               240

/** @brief SPI pixel clock frequency in Hertz. */
#define LCD_PIXEL_CLOCK_HZ      (10 * 1000 * 1000)

/* =========================================================================
 * I2C Sensors (GY906 Contactless Temperature / BME680)
 * ========================================================================= */

#define I2C_MASTER_NUM          0
#define I2C_PIN_SDA             7
#define I2C_PIN_SCL             9
#define I2C_MASTER_FREQ_HZ      100000

/* =========================================================================
 * System Control & Input Buttons
 * ========================================================================= */

/** @brief Capture / Stream trigger button (Active-Low switch to GND). */
#define PIN_BTN_ENABLE          42

/* =========================================================================
 * OV5640 CSP / DVP Camera Interface Wiring (esp_camera.h)
 * ========================================================================= */

#ifndef CAM_PIN_PWDN
#define CAM_PIN_PWDN            -1
#endif

#ifndef CAM_PIN_RESET
#define CAM_PIN_RESET           -1
#endif

#ifndef CAM_PIN_XCLK
#define CAM_PIN_XCLK            15
#endif

#ifndef CAM_PIN_SIOD
#define CAM_PIN_SIOD            4
#endif

#ifndef CAM_PIN_SIOC
#define CAM_PIN_SIOC            5
#endif

#ifndef CAM_PIN_D7
#define CAM_PIN_D7              16
#endif

#ifndef CAM_PIN_D6
#define CAM_PIN_D6              17
#endif

#ifndef CAM_PIN_D5
#define CAM_PIN_D5              18
#endif

#ifndef CAM_PIN_D4
#define CAM_PIN_D4              12
#endif

#ifndef CAM_PIN_D3
#define CAM_PIN_D3              10
#endif

#ifndef CAM_PIN_D2
#define CAM_PIN_D2              8
#endif

#ifndef CAM_PIN_D1
#define CAM_PIN_D1              9
#endif

#ifndef CAM_PIN_D0
#define CAM_PIN_D0              11
#endif

#ifndef CAM_PIN_VSYNC
#define CAM_PIN_VSYNC           6
#endif

#ifndef CAM_PIN_HREF
#define CAM_PIN_HREF            7
#endif

#ifndef CAM_PIN_PCLK
#define CAM_PIN_PCLK            13
#endif