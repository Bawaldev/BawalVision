#include "camera_task.h"

#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "pins.h"

static const char *TAG = "CAMERA_OV5640";

static bool s_is_initialized = false;
static char s_detected_camera_model[32] = "OV5640";

bool camera_task_is_ready(void) {
  return s_is_initialized;
}

const char *camera_get_sensor_name(void) {
  return s_detected_camera_model;
}

esp_err_t camera_task_init(void) {
  if (s_is_initialized)
    return ESP_OK;

  camera_config_t config = {
      .pin_pwdn = CAM_PIN_PWDN,
      .pin_reset = CAM_PIN_RESET,
      .pin_xclk = CAM_PIN_XCLK,
      .pin_sccb_sda = CAM_PIN_SIOD,
      .pin_sccb_scl = CAM_PIN_SIOC,

      .pin_d7 = CAM_PIN_D7,
      .pin_d6 = CAM_PIN_D6,
      .pin_d5 = CAM_PIN_D5,
      .pin_d4 = CAM_PIN_D4,
      .pin_d3 = CAM_PIN_D3,
      .pin_d2 = CAM_PIN_D2,
      .pin_d1 = CAM_PIN_D1,
      .pin_d0 = CAM_PIN_D0,
      .pin_vsync = CAM_PIN_VSYNC,
      .pin_href = CAM_PIN_HREF,
      .pin_pclk = CAM_PIN_PCLK,

      .xclk_freq_hz = 12000000,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,

      .pixel_format = PIXFORMAT_JPEG,
      .frame_size = FRAMESIZE_QVGA,
      .jpeg_quality = 24,
      .fb_count = 2,
      .fb_location = CAMERA_FB_IN_PSRAM,
      .grab_mode = CAMERA_GRAB_LATEST,
  };

  ESP_LOGI(TAG, "Initializing OV5640 CSP via esp_camera_init...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Camera init failed with error: %s (0x%x)", esp_err_to_name(err), err);
    return err;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    if (s->id.PID == 0x5640 || (s->id.PID >> 8) == 0x56) {
      strncpy(s_detected_camera_model, "OV5640", sizeof(s_detected_camera_model) - 1);
    } else if (s->id.PID == 0x3660 || (s->id.PID >> 8) == 0x36) {
      strncpy(s_detected_camera_model, "OV3660", sizeof(s_detected_camera_model) - 1);
    } else if (s->id.PID == 0x2642 || s->id.PID == 0x2640 || (s->id.PID >> 8) == 0x26) {
      strncpy(s_detected_camera_model, "OV2640", sizeof(s_detected_camera_model) - 1);
    } else if (s->id.PID == 0x7721 || (s->id.PID >> 8) == 0x77) {
      strncpy(s_detected_camera_model, "OV7725", sizeof(s_detected_camera_model) - 1);
    } else {
      snprintf(s_detected_camera_model, sizeof(s_detected_camera_model), "SENSOR_0x%04X",
               (unsigned int)s->id.PID);
    }
    ESP_LOGI(TAG, "Camera sensor: %s (PID: 0x%04x)", s_detected_camera_model,
             (unsigned int)s->id.PID);

    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, 24);

    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);

    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_ae_level(s, 0);
    s->set_gain_ctrl(s, 1);
    s->set_gainceiling(s, GAINCEILING_16X);

    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_dcw(s, 0);

    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_sharpness(s, 0);
  }

  s_is_initialized = true;
  ESP_LOGI(TAG, "JPEG QVGA @ 25 FPS, 2x PSRAM DMA buffers, CAMERA_GRAB_LATEST");
  return ESP_OK;
}
