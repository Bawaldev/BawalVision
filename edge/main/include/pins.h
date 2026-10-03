#pragma once

#include "esp_chip_info.h"
#include "sdkconfig.h"

/**
 * @file pins.h
 * @brief Hardware pin mapping and peripheral assignments for BawalVision edge node.
 * Architecture: ESP32-S3-CAM + OV3660 + ST7789 TFT 1.3" + GY906 (BawalVision v3 Schematic)
 */

/* =========================================================================
 * ST7789 SPI LCD Display Wiring (TFT 1.3" 240x240)
 * ========================================================================= */

/** @brief SPI controller host used by the display panel (SPI2_HOST = FSPI). */
#define LCD_HOST                SPI2_HOST

/** @brief Master Out Slave In (SDA/MOSI) data line to LCD (GPIO14 / Pin 19). */
#define LCD_PIN_MOSI            14

/** @brief SPI clock line (SCL/SCK) (GPIO21 / Pin 37-38). */
#define LCD_PIN_SCK             21

/** @brief Data / Command control pin (GPIO45 / Pin 34). */
#define LCD_PIN_DC              45

/** @brief Panel reset pin (-1: tied to ESP EN/RES pin in schematic). */
#define LCD_PIN_RST             -1

/** @brief SPI chip select pin (-1: CS tied directly to GND in schematic). */
#define LCD_PIN_CS              -1

/** @brief Backlight control pin (-1: BLK tied directly to 3V3 in schematic). */
#define LCD_PIN_BCKL            -1

/** @brief Horizontal resolution in pixels. */
#define LCD_H_RES               240

/** @brief Vertical resolution in pixels. */
#define LCD_V_RES               240

/** @brief SPI pixel clock frequency in Hertz (20 MHz). */
#define LCD_PIXEL_CLOCK_HZ      (20 * 1000 * 1000)

/* =========================================================================
 * I2C Sensors (GY906 MLX90614 Contactless IR Temperature)
 * ========================================================================= */

#define I2C_MASTER_NUM          0
#define I2C_PIN_SDA             1   /* GPIO 1 / Pin 23 */
#define I2C_PIN_SCL             47  /* GPIO 47 / Pin 36 */
#define I2C_MASTER_FREQ_HZ      100000

/* =========================================================================
 * System Control & Status
 * ========================================================================= */

/** @brief Boot button (GPIO 0 / Pin 33). */
#define PIN_BTN_BOOT            0

/** @brief Status / Flash LED (GPIO 2 / Pin 24). */
#define PIN_LED_STATUS          2

/* =========================================================================
 * OV3660 / OV5640 DVP Camera Interface Wiring (esp_camera.h)
 * ========================================================================= */

#ifndef CAM_PIN_PWDN
#define CAM_PIN_PWDN            -1  /* PWDN tied to GND */
#endif

#ifndef CAM_PIN_RESET
#define CAM_PIN_RESET           -1  /* RESET tied to 3V3 */
#endif

#ifndef CAM_PIN_XCLK
#define CAM_PIN_XCLK            15  /* IO15 / Pin 7 */
#endif

#ifndef CAM_PIN_SIOD
#define CAM_PIN_SIOD            4   /* IO4 / Pin 3 */
#endif

#ifndef CAM_PIN_SIOC
#define CAM_PIN_SIOC            5   /* IO5 / Pin 4 */
#endif

#ifndef CAM_PIN_D7
#define CAM_PIN_D7              16  /* CAM_Y9 -> IO16 / Pin 8 */
#endif

#ifndef CAM_PIN_D6
#define CAM_PIN_D6              17  /* CAM_Y8 -> IO17 / Pin 9 */
#endif

#ifndef CAM_PIN_D5
#define CAM_PIN_D5              18  /* CAM_Y7 -> IO18 / Pin 10 */
#endif

#ifndef CAM_PIN_D4
#define CAM_PIN_D4              12  /* CAM_Y6 -> IO12 / Pin 17 */
#endif

#ifndef CAM_PIN_D3
#define CAM_PIN_D3              10  /* CAM_Y5 -> IO10 / Pin 15 */
#endif

#ifndef CAM_PIN_D2
#define CAM_PIN_D2              8   /* CAM_Y4 -> IO8 / Pin 11 */
#endif

#ifndef CAM_PIN_D1
#define CAM_PIN_D1              9   /* CAM_Y3 -> IO9 / Pin 14 */
#endif

#ifndef CAM_PIN_D0
#define CAM_PIN_D0              11  /* CAM_Y2 -> IO11 / Pin 16 */
#endif

#ifndef CAM_PIN_VSYNC
#define CAM_PIN_VSYNC           6   /* IO6 / Pin 5 */
#endif

#ifndef CAM_PIN_HREF
#define CAM_PIN_HREF            7   /* IO7 / Pin 6 */
#endif

#ifndef CAM_PIN_PCLK
#define CAM_PIN_PCLK            13  /* IO13 / Pin 18 */
#endif