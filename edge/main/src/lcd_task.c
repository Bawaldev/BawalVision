#include "lcd_task.h"
#include "pins.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "LCD_TASK";

static lv_disp_t *s_disp_handle = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_status_card = NULL;
static system_status_t s_current_status = SYSTEM_STATUS_IDLE;

const char *lcd_status_to_string(system_status_t status) {
  switch (status) {
  case SYSTEM_STATUS_IDLE:
    return "IDLE";
  case SYSTEM_STATUS_PASS:
    return "PASS";
  case SYSTEM_STATUS_FAIL:
    return "FAIL";
  default:
    return "UNKNOWN";
  }
}

const char *lcd_get_chip_type_string(void) {
  static char chip_desc[64] = {0};
  if (chip_desc[0] != '\0') {
    return chip_desc;
  }

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  const char *model_name = "ESP32";
  switch (chip_info.model) {
  case CHIP_ESP32:
    model_name = "ESP32";
    break;
  case CHIP_ESP32S2:
    model_name = "ESP32-S2";
    break;
  case CHIP_ESP32S3:
    model_name = "ESP32-S3";
    break;
  case CHIP_ESP32C3:
    model_name = "ESP32-C3";
    break;
  case CHIP_ESP32C2:
    model_name = "ESP32-C2";
    break;
  case CHIP_ESP32C6:
    model_name = "ESP32-C6";
    break;
  case CHIP_ESP32H2:
    model_name = "ESP32-H2";
    break;
  default:
    model_name = CONFIG_IDF_TARGET;
    break;
  }

  snprintf(chip_desc, sizeof(chip_desc), "%s (r%d, %dC)", model_name,
           chip_info.revision, chip_info.cores);
  return chip_desc;
}

static void apply_status_style(system_status_t status) {
  if (!s_status_card || !s_status_label) {
    return;
  }

  lv_color_t bg_color;
  switch (status) {
  case SYSTEM_STATUS_PASS:
    bg_color = lv_palette_main(LV_PALETTE_GREEN);
    break;
  case SYSTEM_STATUS_FAIL:
    bg_color = lv_palette_main(LV_PALETTE_RED);
    break;
  case SYSTEM_STATUS_IDLE:
  default:
    bg_color = lv_palette_main(LV_PALETTE_AMBER);
    break;
  }

  lv_obj_set_style_bg_color(s_status_card, bg_color, 0);
  lv_label_set_text_fmt(s_status_label, "STATUS: %s",
                        lcd_status_to_string(status));
}

void lcd_update_status(system_status_t status) {
  s_current_status = status;
  if (lvgl_port_lock(0)) {
    apply_status_style(status);
    lvgl_port_unlock();
  }
}

static void create_ui(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f172a),
                            0); // Dark slate background

  // Main layout container
  lv_obj_t *cont = lv_obj_create(scr);
  lv_obj_set_size(cont, LCD_H_RES, LCD_V_RES);
  lv_obj_center(cont);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 8, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // Title
  lv_obj_t *title_label = lv_label_create(cont);
  lv_label_set_text(title_label, "BAWAL VISION");
  lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_CYAN), 0);

  // Chip Info Card
  lv_obj_t *chip_card = lv_obj_create(cont);
  lv_obj_set_width(chip_card, lv_pct(90));
  lv_obj_set_height(chip_card, 48);
  lv_obj_set_style_bg_color(chip_card, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_color(chip_card, lv_color_hex(0x334155), 0);
  lv_obj_set_style_border_width(chip_card, 1, 0);
  lv_obj_set_style_radius(chip_card, 6, 0);
  lv_obj_set_style_pad_all(chip_card, 4, 0);

  lv_obj_t *chip_label = lv_label_create(chip_card);
  lv_label_set_text_fmt(chip_label, "CHIP: %s", lcd_get_chip_type_string());
  lv_obj_set_style_text_color(chip_label, lv_color_white(), 0);
  lv_obj_center(chip_label);

  s_status_card = lv_obj_create(cont);
  lv_obj_set_width(s_status_card, lv_pct(90));
  lv_obj_set_height(s_status_card, 48);
  lv_obj_set_style_border_width(s_status_card, 0, 0);
  lv_obj_set_style_radius(s_status_card, 6, 0);
  lv_obj_set_style_pad_all(s_status_card, 4, 0);

  s_status_label = lv_label_create(s_status_card);
  lv_obj_set_style_text_color(s_status_label, lv_color_black(), 0);
  lv_obj_center(s_status_label);

  apply_status_style(s_current_status);
}

void lcd_task(void *pvParameters) {
  ESP_LOGI(TAG, "Initializing ST7789 SPI LCD Display on SPI host %d", LCD_HOST);

  spi_bus_config_t buscfg = {
      .sclk_io_num = LCD_PIN_SCK,
      .mosi_io_num = LCD_PIN_MOSI,
      .miso_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
  };
  esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
    return;
  }

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

  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = LCD_PIN_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  // Optional Backlight GPIO
#if (LCD_PIN_BCKL >= 0)
  gpio_config_t bk_gpio_config = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = 1ULL << LCD_PIN_BCKL,
  };
  gpio_config(&bk_gpio_config);
  gpio_set_level(LCD_PIN_BCKL, 1);
#endif

  // 4. Initialize LVGL Port
  lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

  lvgl_port_display_cfg_t disp_cfg = {.io_handle = io_handle,
                                      .panel_handle = panel_handle,
                                      .buffer_size = LCD_H_RES * 20,
                                      .double_buffer = true,
                                      .hres = LCD_H_RES,
                                      .vres = LCD_V_RES,
                                      .monochrome = false,
                                      .rotation =
                                          {
                                              .swap_xy = false,
                                              .mirror_x = false,
                                              .mirror_y = false,
                                          },
                                      .flags = {
                                          .buff_dma = true,
                                      }};
  s_disp_handle = lvgl_port_add_disp(&disp_cfg);
  if (!s_disp_handle) {
    ESP_LOGE(TAG, "Failed to register LVGL display handle");
    vTaskDelete(NULL);
    return;
  }

  // 5. Render UI
  if (lvgl_port_lock(0)) {
    create_ui();
    lvgl_port_unlock();
  }

  ESP_LOGI(
      TAG,
      "LCD Task initialized successfully. ESP Type: %s, Initial Status: %s",
      lcd_get_chip_type_string(), lcd_status_to_string(s_current_status));

  // Main loop: keep task alive or monitor status
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
