#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd_task.h"
#include "pins.h"
#include "system_monitor.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "LCD_TASK";

static lv_disp_t *s_disp_handle = NULL;
static lv_obj_t *s_http_bar = NULL;
static lv_obj_t *s_http_label = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_status_card = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_esp_temp_label = NULL;
static lv_obj_t *s_cpu_load_label = NULL;
static lv_obj_t *s_uptime_label = NULL;

static system_status_t s_current_status = SYSTEM_STATUS_IDLE;
static bool s_wifi_connected = false;
static char s_wifi_ip_str[24] = "DISCONNECTED";

const char *lcd_status_to_string(system_status_t status) {
  switch (status) {
  case SYSTEM_STATUS_IDLE:
    return "IDLE";
  case SYSTEM_STATUS_PASS:
    return "PASS";
  case SYSTEM_STATUS_FAIL:
    return "REJECT";
  case SYSTEM_STATUS_REVIEW:
    return "REVIEW";
  default:
    return "UNKNOWN";
  }
}

const char *lcd_get_chip_type_string(void) {
  static char chip_desc[48] = {0};
  if (chip_desc[0] != '\0') {
    return chip_desc;
  }

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  const char *model_name = "ESP32-S3";
  switch (chip_info.model) {
  case CHIP_ESP32S3:
    model_name = "ESP32-S3";
    break;
  case CHIP_ESP32:
    model_name = "ESP32";
    break;
  case CHIP_ESP32C3:
    model_name = "ESP32-C3";
    break;
  default:
    model_name = "ESP32-S3";
    break;
  }

  snprintf(chip_desc, sizeof(chip_desc), "%s r%d", model_name,
           chip_info.revision);
  return chip_desc;
}

static void apply_status_style(system_status_t status) {
  if (!s_status_card || !s_status_label) {
    return;
  }

  lv_color_t bg_color;
  switch (status) {
  case SYSTEM_STATUS_PASS:
    bg_color = lv_color_hex(0x10b981); // Emerald Green
    break;
  case SYSTEM_STATUS_FAIL:
    bg_color = lv_color_hex(0xf43f5e); // Rose Red
    break;
  case SYSTEM_STATUS_REVIEW:
    bg_color = lv_color_hex(0xf97316); // Amber/Orange
    break;
  case SYSTEM_STATUS_IDLE:
  default:
    bg_color = lv_color_hex(0x0ea5e9); // Sky Blue
    break;
  }

  lv_obj_set_style_bg_color(s_status_card, bg_color, 0);
  lv_label_set_text(s_status_label, lcd_status_to_string(status));
  lv_obj_invalidate(s_status_card);
}

void lcd_update_status(system_status_t status) {
  s_current_status = status;
  if (lvgl_port_lock(0)) {
    apply_status_style(status);
    lvgl_port_unlock();
  }
}

void lcd_update_http_status(const char *status_str, bool is_error) {
  if (!s_http_bar || !s_http_label || status_str == NULL) {
    return;
  }

  if (lvgl_port_lock(0)) {
    lv_color_t bar_bg =
        is_error ? lv_color_hex(0xe11d48) : lv_color_hex(0x0369a1);
    if (strstr(status_str, "200") != NULL || strstr(status_str, "OK") != NULL) {
      bar_bg = lv_color_hex(0x059669); // Green
    } else if (strstr(status_str, "SEND") != NULL ||
               strstr(status_str, "POST") != NULL) {
      bar_bg = lv_color_hex(0xd97706); // Amber
    }

    lv_obj_set_style_bg_color(s_http_bar, bar_bg, 0);
    lv_label_set_text(s_http_label, status_str);
    lv_obj_invalidate(s_http_bar);
    lvgl_port_unlock();
  }
}

void lcd_update_wifi_status(bool connected, const char *ip_address) {
  s_wifi_connected = connected;
  if (ip_address && strlen(ip_address) > 0) {
    strncpy(s_wifi_ip_str, ip_address, sizeof(s_wifi_ip_str) - 1);
    s_wifi_ip_str[sizeof(s_wifi_ip_str) - 1] = '\0';
  } else {
    strncpy(s_wifi_ip_str, connected ? "CONNECTED" : "DISCONNECTED",
            sizeof(s_wifi_ip_str) - 1);
  }

  if (s_wifi_label && lvgl_port_lock(0)) {
    if (s_wifi_connected) {
      lv_label_set_text_fmt(s_wifi_label, "#10b981 # %s", s_wifi_ip_str);
    } else {
      lv_label_set_text_fmt(s_wifi_label, "#f43f5e # NO WIFI");
    }
    lvgl_port_unlock();
  }
}

void lcd_update_telemetry_display(float temperature, uint8_t cpu_load) {
  if (lvgl_port_lock(0)) {
    if (s_esp_temp_label) {
      lv_label_set_text_fmt(s_esp_temp_label, "TEMP: %.1f C",
                            temperature > 0 ? temperature : 36.5f);
    }
    if (s_cpu_load_label) {
      lv_label_set_text_fmt(s_cpu_load_label, "CPU : %u%%",
                            (unsigned int)cpu_load);
    }
    lvgl_port_unlock();
  }
}

static void create_dashboard_ui(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Root container (128x160)
  lv_obj_t *root = lv_obj_create(scr);
  lv_obj_set_size(root, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_set_style_pad_row(root, 0, 0);
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  // 1. TOP HTTP STATUS BAR (Width: LCD_H_RES, Height: 18)
  s_http_bar = lv_obj_create(root);
  lv_obj_set_size(s_http_bar, LCD_H_RES, 18);
  lv_obj_set_style_bg_color(s_http_bar, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_http_bar, 0, 0);
  lv_obj_set_style_radius(s_http_bar, 0, 0);
  lv_obj_set_style_pad_all(s_http_bar, 1, 0);
  lv_obj_clear_flag(s_http_bar, LV_OBJ_FLAG_SCROLLABLE);

  s_http_label = lv_label_create(s_http_bar);
  lv_label_set_text(s_http_label, "HTTP: IDLE");
  lv_obj_set_style_text_color(s_http_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_http_label, LV_FONT_DEFAULT, 0);
  lv_obj_center(s_http_label);

  // 2. Wi-Fi & Device Sub-bar (Height: 12)
  s_wifi_label = lv_label_create(root);
  lv_label_set_recolor(s_wifi_label, true);
  lv_label_set_text(s_wifi_label, "#f43f5e # CONNECTING...");
  lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x94a3b8), 0);
  lv_obj_set_style_text_font(s_wifi_label, LV_FONT_DEFAULT, 0);

  // 3. System Inspection Status Card (Width: 120, Height: 26)
  s_status_card = lv_obj_create(root);
  lv_obj_set_size(s_status_card, LCD_H_RES - 8, 26);
  lv_obj_set_style_bg_color(s_status_card, lv_color_hex(0x0ea5e9), 0);
  lv_obj_set_style_border_width(s_status_card, 0, 0);
  lv_obj_set_style_radius(s_status_card, 0, 0);
  lv_obj_set_style_pad_all(s_status_card, 1, 0);
  lv_obj_clear_flag(s_status_card, LV_OBJ_FLAG_SCROLLABLE);

  s_status_label = lv_label_create(s_status_card);
  lv_label_set_text(s_status_label, "BAWAL VISION");
  lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
  lv_obj_center(s_status_label);

  // 4. Multi-Sensor Data Box (Width: 120, Height: 36)
  lv_obj_t *sensor_box = lv_obj_create(root);
  lv_obj_set_size(sensor_box, LCD_H_RES - 8, 36);
  lv_obj_set_style_bg_color(sensor_box, lv_color_hex(0x161e2e), 0);
  lv_obj_set_style_border_color(sensor_box, lv_color_hex(0x2d3748), 0);
  lv_obj_set_style_border_width(sensor_box, 1, 0);
  lv_obj_set_style_radius(sensor_box, 0, 0);
  lv_obj_set_style_pad_all(sensor_box, 2, 0);
  lv_obj_set_flex_flow(sensor_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sensor_box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(sensor_box, LV_OBJ_FLAG_SCROLLABLE);

  s_esp_temp_label = lv_label_create(sensor_box);
  lv_label_set_text(s_esp_temp_label, "ESP32: -- C");
  lv_obj_set_style_text_color(s_esp_temp_label, lv_color_hex(0xfb923c),
                              0); // Orange
  lv_obj_set_style_text_font(s_esp_temp_label, LV_FONT_DEFAULT, 0);

  s_cpu_load_label = lv_label_create(sensor_box);
  lv_label_set_text(s_cpu_load_label, "CPU : --%");
  lv_obj_set_style_text_color(s_cpu_load_label, lv_color_hex(0x38bdf8),
                              0); // Cyan
  lv_obj_set_style_text_font(s_cpu_load_label, LV_FONT_DEFAULT, 0);

  // 5. Bottom Heartbeat & Uptime Banner (Height: 12)
  s_uptime_label = lv_label_create(root);
  lv_label_set_text(s_uptime_label, "UP 00:00:00");
  lv_obj_set_style_text_color(s_uptime_label, lv_color_hex(0x94a3b8), 0);
  lv_obj_set_style_text_font(s_uptime_label, LV_FONT_DEFAULT, 0);

  apply_status_style(s_current_status);
}

void lcd_monitor_heartbeat_task(void *pvParameters) {
  uint32_t seconds = 0;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    seconds++;

    uint32_t hrs = seconds / 3600;
    uint32_t mins = (seconds % 3600) / 60;
    uint32_t secs = seconds % 60;

    float real_temp = get_esp32_cpu_temperature();
    uint8_t cpu_load = get_cpu_usage_percentage();

    if (lvgl_port_lock(0)) {
      if (s_uptime_label) {
        lv_label_set_text_fmt(s_uptime_label, "UP %02u:%02u:%02u",
                              (unsigned int)hrs, (unsigned int)mins,
                              (unsigned int)secs);
      }
      if (s_esp_temp_label) {
        lv_label_set_text_fmt(s_esp_temp_label, "ESP32: %.1f C", real_temp);
      }
      if (s_cpu_load_label) {
        lv_label_set_text_fmt(s_cpu_load_label, "CPU : %u%%",
                              (unsigned int)cpu_load);
      }
      lvgl_port_unlock();
    }
  }
}

void lcd_task(void *pvParameters) {
  ESP_LOGI(TAG,
           "Initializing ST7789/ST7735 128x160 SPI TFT on host %d (MOSI:%d, "
           "SCK:%d, DC:%d, CS:%d)",
           LCD_HOST, LCD_PIN_MOSI, LCD_PIN_SCK, LCD_PIN_DC, LCD_PIN_CS);

// 1. Initialize SPI Bus & Backlight
#if (LCD_PIN_BCKL >= 0)
  gpio_config_t bk_gpio_config = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = 1ULL << LCD_PIN_BCKL,
  };
  gpio_config(&bk_gpio_config);
  gpio_set_level(LCD_PIN_BCKL, 1);
#endif

  spi_bus_config_t buscfg = {
      .sclk_io_num = LCD_PIN_SCK,
      .mosi_io_num = LCD_PIN_MOSI,
      .miso_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = LCD_H_RES * 20 * sizeof(uint16_t),
  };
  esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
    vTaskDelete(NULL);
    return;
  }

  // 2. Configure Panel IO with SPI Mode 0 & 20MHz
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

  // 3. Configure Panel Controller with RGB Color Element Order
  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = LCD_PIN_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, // RGB Colorization
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));

  // Software reset and display init
  esp_lcd_panel_io_tx_param(io_handle, 0x01, NULL, 0);
  vTaskDelay(pdMS_TO_TICKS(120));

  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  esp_lcd_panel_set_gap(panel_handle, 0, 0);
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  ESP_LOGI(TAG, "128x160 TFT panel initialized (RGB mode, %dx%d).", LCD_H_RES,
           LCD_V_RES);

  // 4. Initialize LVGL Port
  lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

  lvgl_port_display_cfg_t disp_cfg = {.io_handle = io_handle,
                                      .panel_handle = panel_handle,
                                      .buffer_size = LCD_H_RES * 16,
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

  // 5. Render Modern 128x160 Dashboard UI
  if (lvgl_port_lock(0)) {
    create_dashboard_ui();
    lvgl_port_unlock();
  }

// 6. Start Heartbeat / Connection Monitor Task (Static Allocation)
#define LCD_HB_TASK_STACK_SIZE 3072
  static StaticTask_t s_lcd_hb_task_buffer;
  static StackType_t s_lcd_hb_task_stack[LCD_HB_TASK_STACK_SIZE];

  xTaskCreateStaticPinnedToCore(lcd_monitor_heartbeat_task, "lcd_heartbeat",
                                LCD_HB_TASK_STACK_SIZE, NULL, 2,
                                s_lcd_hb_task_stack, &s_lcd_hb_task_buffer, 0);

  ESP_LOGI(TAG, "128x160 LVGL Dashboard active.");
  vTaskDelete(NULL);
}