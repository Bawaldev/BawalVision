#include "lcd_min_test.h"
#include "pins.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCD_MIN_TEST";

#define FRAME_PIXELS (LCD_H_RES * LCD_V_RES)
static uint16_t *s_frame_buffer = NULL;

static void draw_full_frame(esp_lcd_panel_handle_t panel, uint16_t fill_color, uint16_t border_color) {
  if (!s_frame_buffer) {
    ESP_LOGE(TAG, "Frame buffer not allocated!");
    return;
  }

  // Pre-swap bytes for ST7789 Big-Endian SPI format
  uint16_t be_fill = (uint16_t)((fill_color >> 8) | ((fill_color & 0xFF) << 8));
  uint16_t be_border = (uint16_t)((border_color >> 8) | ((border_color & 0xFF) << 8));

  for (int y = 0; y < LCD_V_RES; y++) {
    for (int x = 0; x < LCD_H_RES; x++) {
      int idx = y * LCD_H_RES + x;
      if (x < 4 || x >= (LCD_H_RES - 4) || y < 4 || y >= (LCD_V_RES - 4)) {
        s_frame_buffer[idx] = be_border;
      } else {
        s_frame_buffer[idx] = be_fill;
      }
    }
  }

  esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, s_frame_buffer);
}

void lcd_min_test_task(void *pvParameters) {
  ESP_LOGI(TAG, "=================================================");
  ESP_LOGI(TAG, "  FULL-FRAME ATOMIC ST7789 TEST (%dx%d)", LCD_H_RES, LCD_V_RES);
  ESP_LOGI(TAG, "  MOSI:%d | SCK:%d | DC:%d | RST:%d | CS:%d",
           LCD_PIN_MOSI, LCD_PIN_SCK, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_CS);
  ESP_LOGI(TAG, "=================================================");

  // 1. Allocate Frame Buffer in DMA SRAM
  s_frame_buffer = heap_caps_malloc(FRAME_PIXELS * sizeof(uint16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!s_frame_buffer) {
    s_frame_buffer = heap_caps_malloc(FRAME_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
  }
  if (!s_frame_buffer) {
    ESP_LOGE(TAG, "FATAL: Could not allocate frame buffer!");
    vTaskDelete(NULL);
    return;
  }

  // 2. Hardware Reset Pulse
#if (LCD_PIN_RST >= 0)
  gpio_config_t rst_conf = {
      .pin_bit_mask = (1ULL << LCD_PIN_RST),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&rst_conf);
  gpio_set_level(LCD_PIN_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(LCD_PIN_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(150));
#endif

  // 3. Backlight Setup
#if (LCD_PIN_BCKL >= 0)
  gpio_config_t bk_conf = {
      .pin_bit_mask = (1ULL << LCD_PIN_BCKL),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&bk_conf);
  gpio_set_level(LCD_PIN_BCKL, 1);
#endif

  // 4. SPI Bus Initialization
  spi_bus_config_t buscfg = {
      .sclk_io_num = LCD_PIN_SCK,
      .mosi_io_num = LCD_PIN_MOSI,
      .miso_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = FRAME_PIXELS * sizeof(uint16_t),
  };
  esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
    return;
  }

  // 5. Panel IO Configuration
  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = LCD_PIN_DC,
      .cs_gpio_num = LCD_PIN_CS,
      .pclk_hz = LCD_PIXEL_CLOCK_HZ,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 0,
      .trans_queue_depth = 10,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                           &io_config, &io_handle));

  // 6. Panel Controller Setup
  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = -1,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  // 7. Loop Frame Pattern Colors
  const struct {
    const char *name;
    uint16_t color;
    uint16_t border;
  } patterns[] = {
      {"RED with WHITE border", 0xF800, 0xFFFF},
      {"GREEN with RED border", 0x07E0, 0xF800},
      {"BLUE with YELLOW border", 0x001F, 0xFFE0},
      {"WHITE with BLUE border", 0xFFFF, 0x001F},
      {"YELLOW with BLACK border", 0xFFE0, 0x0000},
      {"CYAN with MAGENTA border", 0x07FF, 0xF81F},
      {"BLACK with WHITE border", 0x0000, 0xFFFF},
  };
  const int count = sizeof(patterns) / sizeof(patterns[0]);

  int idx = 0;
  while (1) {
    ESP_LOGI(TAG, "Drawing: [%s]", patterns[idx].name);
    draw_full_frame(panel_handle, patterns[idx].color, patterns[idx].border);
    vTaskDelay(pdMS_TO_TICKS(1500));
    idx = (idx + 1) % count;
  }
}

esp_err_t lcd_min_test_run(void) {
  lcd_min_test_task(NULL);
  return ESP_OK;
}