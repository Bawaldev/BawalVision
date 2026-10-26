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

// 1. Connection Header Widget
static lv_obj_t *s_conn_bar = NULL;
static lv_obj_t *s_conn_badge = NULL;
static lv_obj_t *s_conn_label = NULL;
static lv_obj_t *s_latency_label = NULL;

// 2. Wi-Fi & Device Sub-bar
static lv_obj_t *s_wifi_label = NULL;

// 3. System Inspection Status Card
static lv_obj_t *s_status_card = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_status_sub = NULL;

// 4. Industrial Telemetry Box
static lv_obj_t *s_speed_label = NULL;
static lv_obj_t *s_esp_temp_label = NULL;
static lv_obj_t *s_cpu_load_label = NULL;

// 5. Bottom Uptime & Status Banner
static lv_obj_t *s_uptime_label = NULL;

static system_status_t s_current_status = SYSTEM_STATUS_IDLE;
static bool s_is_connected = false;
static uint32_t s_current_latency_ms = 0;
static float s_current_line_speed = 0.35f; // default 0.35 m/s conveyor line rate
static char s_wifi_ip_str[24] = "DISCONNECTED";
static volatile bool s_lvgl_ready = false;

const char *lcd_status_to_string(system_status_t status) {
  switch (status) {
  case SYSTEM_STATUS_IDLE:
    return "READY";
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
  const char *sub_txt = "FOOD INSPECT";
  switch (status) {
  case SYSTEM_STATUS_PASS:
    bg_color = lv_color_hex(0x10b981); // Emerald Green
    sub_txt = "TRAY PASSED";
    break;
  case SYSTEM_STATUS_FAIL:
    bg_color = lv_color_hex(0xf43f5e); // Rose Red
    sub_txt = "TRAY REJECTED";
    break;
  case SYSTEM_STATUS_REVIEW:
    bg_color = lv_color_hex(0xf97316); // Amber/Orange
    sub_txt = "MANUAL REVIEW";
    break;
  case SYSTEM_STATUS_IDLE:
  default:
    bg_color = lv_color_hex(0x0284c7); // Sky Blue
    sub_txt = "BAWAL VISION QC";
    break;
  }

  lv_obj_set_style_bg_color(s_status_card, bg_color, 0);
  lv_label_set_text(s_status_label, lcd_status_to_string(status));
  if (s_status_sub) {
    lv_label_set_text(s_status_sub, sub_txt);
  }
  lv_obj_invalidate(s_status_card);
}

void lcd_update_status(system_status_t status) {
  if (!s_lvgl_ready) return;
  s_current_status = status;
  if (lvgl_port_lock(0)) {
    apply_status_style(status);
    lvgl_port_unlock();
  }
}

void lcd_update_connection_status(bool connected, uint32_t latency_ms) {
  if (!s_lvgl_ready) return;
  s_is_connected = connected;
  s_current_latency_ms = latency_ms;

  if (lvgl_port_lock(0)) {
    if (s_conn_badge && s_conn_label) {
      lv_color_t badge_bg = connected ? lv_color_hex(0x10b981) : lv_color_hex(0xe11d48);
      lv_obj_set_style_bg_color(s_conn_badge, badge_bg, 0);
      lv_label_set_text(s_conn_label, connected ? "ONLINE" : "OFFLINE");
      lv_obj_invalidate(s_conn_badge);
    }
    if (s_latency_label) {
      if (connected && latency_ms > 0) {
        lv_label_set_text_fmt(s_latency_label, "%u ms", (unsigned int)latency_ms);
      } else if (connected) {
        lv_label_set_text(s_latency_label, "<15 ms");
      } else {
        lv_label_set_text(s_latency_label, "-- ms");
      }
      lv_obj_invalidate(s_latency_label);
    }
    lvgl_port_unlock();
  }
}

void lcd_update_line_speed(float speed_mps) {
  if (!s_lvgl_ready) return;
  s_current_line_speed = speed_mps;
  if (lvgl_port_lock(0)) {
    if (s_speed_label) {
      lv_label_set_text_fmt(s_speed_label, "SPEED : %.2f m/s", speed_mps);
      lv_obj_invalidate(s_speed_label);
    }
    lvgl_port_unlock();
  }
}

void lcd_update_http_status(const char *status_str, bool is_error) {
  if (!s_lvgl_ready || status_str == NULL) {
    return;
  }

  bool is_ok = (!is_error && (strstr(status_str, "200") != NULL ||
                              strstr(status_str, "OK") != NULL ||
                              strstr(status_str, "READY") != NULL));

  uint32_t lat = 0;
  if (is_ok) {
    lat = s_current_latency_ms ? s_current_latency_ms : 24;
  }
  lcd_update_connection_status(is_ok, lat);
}

void lcd_update_wifi_status(bool connected, const char *ip_address) {
  if (!s_lvgl_ready) return;
  s_is_connected = connected;
  if (ip_address && strlen(ip_address) > 0) {
    strncpy(s_wifi_ip_str, ip_address, sizeof(s_wifi_ip_str) - 1);
    s_wifi_ip_str[sizeof(s_wifi_ip_str) - 1] = '\0';
  } else {
    strncpy(s_wifi_ip_str, connected ? "CONNECTED" : "DISCONNECTED",
            sizeof(s_wifi_ip_str) - 1);
  }

  lcd_update_connection_status(connected, connected ? (s_current_latency_ms ? s_current_latency_ms : 18) : 0);

  if (s_wifi_label && lvgl_port_lock(0)) {
    if (connected) {
      lv_label_set_text_fmt(s_wifi_label, "IP: %s", s_wifi_ip_str);
      lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x38bdf8), 0);
    } else {
      lv_label_set_text(s_wifi_label, "WIFI: DISCONNECTED");
      lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0xf43f5e), 0);
    }
    lv_obj_invalidate(s_wifi_label);
    lvgl_port_unlock();
  }
}

void lcd_update_telemetry_display(float temperature, uint8_t cpu_load) {
  if (!s_lvgl_ready) return;
  if (lvgl_port_lock(0)) {
    if (s_esp_temp_label) {
      lv_label_set_text_fmt(s_esp_temp_label, "TEMP  : %.1f C",
                            temperature > 0 ? temperature : 36.5f);
    }
    if (s_cpu_load_label) {
      lv_label_set_text_fmt(s_cpu_load_label, "CPU   : %u%%",
                            (unsigned int)cpu_load);
    }
    lvgl_port_unlock();
  }
}

static void create_dashboard_ui(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x090d16), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Root container (128x160)
  lv_obj_t *root = lv_obj_create(scr);
  lv_obj_set_size(root, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(root, lv_color_hex(0x090d16), 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_set_style_pad_row(root, 2, 0);
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  // 1. TOP CONNECTION & LATENCY BAR (Width: LCD_H_RES, Height: 20)
  s_conn_bar = lv_obj_create(root);
  lv_obj_set_size(s_conn_bar, LCD_H_RES, 20);
  lv_obj_set_style_bg_color(s_conn_bar, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_border_width(s_conn_bar, 0, 0);
  lv_obj_set_style_radius(s_conn_bar, 0, 0);
  lv_obj_set_style_pad_hor(s_conn_bar, 4, 0);
  lv_obj_set_style_pad_ver(s_conn_bar, 2, 0);
  lv_obj_set_flex_flow(s_conn_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_conn_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(s_conn_bar, LV_OBJ_FLAG_SCROLLABLE);

  // Connection Pill Badge
  s_conn_badge = lv_obj_create(s_conn_bar);
  lv_obj_set_size(s_conn_badge, 56, 16);
  lv_obj_set_style_bg_color(s_conn_badge, lv_color_hex(0xe11d48), 0); // Default OFFLINE Red
  lv_obj_set_style_border_width(s_conn_badge, 0, 0);
  lv_obj_set_style_radius(s_conn_badge, 3, 0);
  lv_obj_set_style_pad_all(s_conn_badge, 0, 0);
  lv_obj_clear_flag(s_conn_badge, LV_OBJ_FLAG_SCROLLABLE);

  s_conn_label = lv_label_create(s_conn_badge);
  lv_label_set_text(s_conn_label, "OFFLINE");
  lv_obj_set_style_text_color(s_conn_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_conn_label, LV_FONT_DEFAULT, 0);
  lv_obj_center(s_conn_label);

  // Latency Label
  s_latency_label = lv_label_create(s_conn_bar);
  lv_label_set_text(s_latency_label, "-- ms");
  lv_obj_set_style_text_color(s_latency_label, lv_color_hex(0x38bdf8), 0); // Cyan
  lv_obj_set_style_text_font(s_latency_label, LV_FONT_DEFAULT, 0);

  // 2. Wi-Fi IP Sub-bar (Height: 14)
  s_wifi_label = lv_label_create(root);
  lv_label_set_text(s_wifi_label, "WIFI: DISCONNECTED");
  lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0xf43f5e), 0);
  lv_obj_set_style_text_font(s_wifi_label, LV_FONT_DEFAULT, 0);

  // 3. System Inspection Status Card (Width: 120, Height: 34)
  s_status_card = lv_obj_create(root);
  lv_obj_set_size(s_status_card, LCD_H_RES - 8, 34);
  lv_obj_set_style_bg_color(s_status_card, lv_color_hex(0x0ea5e9), 0);
  lv_obj_set_style_border_width(s_status_card, 0, 0);
  lv_obj_set_style_radius(s_status_card, 4, 0);
  lv_obj_set_style_pad_all(s_status_card, 1, 0);
  lv_obj_set_flex_flow(s_status_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_status_card, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(s_status_card, LV_OBJ_FLAG_SCROLLABLE);

  s_status_label = lv_label_create(s_status_card);
  lv_label_set_text(s_status_label, "READY");
  lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);

  s_status_sub = lv_label_create(s_status_card);
  lv_label_set_text(s_status_sub, "BAWAL VISION QC");
  lv_obj_set_style_text_color(s_status_sub, lv_color_hex(0xe2e8f0), 0);
  lv_obj_set_style_text_font(s_status_sub, LV_FONT_DEFAULT, 0);

  // 4. Industrial Telemetry Box (Width: 120, Height: 54)
  lv_obj_t *sensor_box = lv_obj_create(root);
  lv_obj_set_size(sensor_box, LCD_H_RES - 8, 54);
  lv_obj_set_style_bg_color(sensor_box, lv_color_hex(0x111827), 0); // Dark Slate
  lv_obj_set_style_border_color(sensor_box, lv_color_hex(0x334155), 0);
  lv_obj_set_style_border_width(sensor_box, 1, 0);
  lv_obj_set_style_radius(sensor_box, 4, 0);
  lv_obj_set_style_pad_all(sensor_box, 3, 0);
  lv_obj_set_style_pad_row(sensor_box, 1, 0);
  lv_obj_set_flex_flow(sensor_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sensor_box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(sensor_box, LV_OBJ_FLAG_SCROLLABLE);

  // Row 1: Line Speed (m/s)
  s_speed_label = lv_label_create(sensor_box);
  lv_label_set_text_fmt(s_speed_label, "SPEED : %.2f m/s", s_current_line_speed);
  lv_obj_set_style_text_color(s_speed_label, lv_color_hex(0xfacc15), 0); // Golden Yellow
  lv_obj_set_style_text_font(s_speed_label, LV_FONT_DEFAULT, 0);

  // Row 2: ESP32 Temperature
  s_esp_temp_label = lv_label_create(sensor_box);
  lv_label_set_text(s_esp_temp_label, "TEMP  : --.- C");
  lv_obj_set_style_text_color(s_esp_temp_label, lv_color_hex(0xfb923c), 0); // Warm Orange
  lv_obj_set_style_text_font(s_esp_temp_label, LV_FONT_DEFAULT, 0);

  // Row 3: CPU Load Percentage
  s_cpu_load_label = lv_label_create(sensor_box);
  lv_label_set_text(s_cpu_load_label, "CPU   : --%");
  lv_obj_set_style_text_color(s_cpu_load_label, lv_color_hex(0x38bdf8), 0); // Cyan Sky
  lv_obj_set_style_text_font(s_cpu_load_label, LV_FONT_DEFAULT, 0);

  // 5. Bottom Heartbeat & Uptime Banner (Height: 14)
  s_uptime_label = lv_label_create(root);
  lv_label_set_text(s_uptime_label, "UP 00:00:00  ● READY");
  lv_obj_set_style_text_color(s_uptime_label, lv_color_hex(0x64748b), 0);
  lv_obj_set_style_text_font(s_uptime_label, LV_FONT_DEFAULT, 0);

  apply_status_style(s_current_status);
  lcd_update_connection_status(s_is_connected, s_current_latency_ms);
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
        lv_label_set_text_fmt(s_uptime_label, "UP %02u:%02u:%02u  %s",
                              (unsigned int)hrs, (unsigned int)mins,
                              (unsigned int)secs,
                              s_is_connected ? "● LINK OK" : "○ OFFLINE");
      }
      if (s_esp_temp_label) {
        lv_label_set_text_fmt(s_esp_temp_label, "TEMP  : %.1f C", real_temp);
      }
      if (s_cpu_load_label) {
        lv_label_set_text_fmt(s_cpu_load_label, "CPU   : %u%%",
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
  s_lvgl_ready = true;

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
