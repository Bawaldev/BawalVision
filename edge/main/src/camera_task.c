#include "camera_task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pins.h"
#include "system_monitor.h"
#include "wifi.h"

static const char *TAG = "CAMERA";

#define FIXED_FRAME_CAPACITY INITIAL_BUF_CAPACITY
// Quality 24 keeps VGA 640x480 frames at ~10-14 KB (prevents Wi-Fi TCP bufferbloat)
#define STREAM_JPEG_QUALITY 24

static frame_slot_t s_slots[2] = {0};
static volatile int s_ready_idx = 0;
static volatile int s_write_idx = 1;
static volatile bool s_has_new_frame = false;

static SemaphoreHandle_t s_camera_mutex = NULL;
static uint32_t s_frame_counter = 0;
static bool s_is_initialized = false;
static atomic_bool s_is_streaming = false;
static atomic_uint_fast64_t s_last_frame_timestamp_ms = 0;
static char s_detected_camera_model[32] = "UNKNOWN";

bool is_camera_streaming(void) {
  if (!atomic_load(&s_is_streaming)) return false;
  uint64_t last = atomic_load(&s_last_frame_timestamp_ms);
  uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
  if (last == 0 || (now - last) > 5000ULL) {
    atomic_store(&s_is_streaming, false);
    return false;
  }
  return true;
}

const char *camera_get_sensor_name(void) {
  return s_detected_camera_model;
}

static void free_slots(void) {
  for (int i = 0; i < 2; i++) {
    if (s_slots[i].data != NULL) {
      free(s_slots[i].data);
      s_slots[i].data = NULL;
    }
  }
}

esp_err_t camera_task_init(void) {
  if (s_is_initialized) return ESP_OK;

  s_camera_mutex = xSemaphoreCreateMutex();
  if (s_camera_mutex == NULL) return ESP_ERR_NO_MEM;

  for (int i = 0; i < 2; i++) {
    s_slots[i].data = (uint8_t *)heap_caps_malloc(
        FIXED_FRAME_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slots[i].data == NULL) {
      s_slots[i].data = (uint8_t *)malloc(FIXED_FRAME_CAPACITY);
    }
    if (s_slots[i].data == NULL) {
      free_slots();
      vSemaphoreDelete(s_camera_mutex);
      s_camera_mutex = NULL;
      return ESP_ERR_NO_MEM;
    }
    memset(&s_slots[i].header, 0, sizeof(camera_frame_header_t));
  }

  camera_config_t config = {
      .pin_pwdn = CAM_PIN_PWDN,
      .pin_reset = CAM_PIN_RESET,
      .pin_xclk = CAM_PIN_XCLK,
      .pin_sccb_sda = CAM_PIN_SIOD,
      .pin_sccb_scl = CAM_PIN_SIOC,
      .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6,
      .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
      .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2,
      .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
      .pin_vsync = CAM_PIN_VSYNC,
      .pin_href = CAM_PIN_HREF,
      .pin_pclk = CAM_PIN_PCLK,
      .xclk_freq_hz = 20000000,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,
      .pixel_format = PIXFORMAT_JPEG,
      .frame_size = FRAMESIZE_VGA,
      .jpeg_quality = STREAM_JPEG_QUALITY,
      .fb_count = 2,
      .fb_location = CAMERA_FB_IN_PSRAM,
      .grab_mode = CAMERA_GRAB_LATEST,
  };

  ESP_LOGI(TAG, "Initializing camera via esp_camera_init...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Camera init failed: %s (0x%x)", esp_err_to_name(err), err);
    free_slots();
    vSemaphoreDelete(s_camera_mutex);
    s_camera_mutex = NULL;
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
      snprintf(s_detected_camera_model, sizeof(s_detected_camera_model),
               "SENSOR_0x%04X", (unsigned int)s->id.PID);
    }
    ESP_LOGI(TAG, "Detected sensor: %s (PID: 0x%04x)",
             s_detected_camera_model, (unsigned int)s->id.PID);

    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    s->set_framesize(s, FRAMESIZE_VGA);
    s->set_quality(s, STREAM_JPEG_QUALITY);

    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    
    // Disable artificial sharpening so VBR does not balloon on motion
    s->set_sharpness(s, 0);

    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_ae_level(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_gainceiling(s, (gainceiling_t)GAINCEILING_4X);

    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_dcw(s, 0);

    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 1);
    if (s->set_denoise) s->set_denoise(s, 1);
  }

  s_is_initialized = true;
  ESP_LOGI(TAG, "Pre-allocated 2x %d KB PSRAM frame buffers OK",
           FIXED_FRAME_CAPACITY / 1024);
  return ESP_OK;
}

static esp_err_t camera_copy_frame(camera_frame_buffer_t *out_frame, bool require_new) {
  if (out_frame == NULL || out_frame->data == NULL) return ESP_ERR_INVALID_ARG;

  if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(15)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  frame_slot_t *slot = &s_slots[s_ready_idx];

  if ((require_new && !s_has_new_frame) || slot->header.buffer_len == 0) {
    xSemaphoreGive(s_camera_mutex);
    return ESP_ERR_NOT_FOUND;
  }

  uint32_t copy_len = slot->header.buffer_len;
  if (copy_len > out_frame->capacity) {
    copy_len = out_frame->capacity;
  }

  memcpy(&out_frame->header, &slot->header, sizeof(camera_frame_header_t));
  memcpy(out_frame->data, slot->data, copy_len);
  out_frame->header.buffer_len = copy_len;
  if (require_new) s_has_new_frame = false;

  xSemaphoreGive(s_camera_mutex);
  return ESP_OK;
}

esp_err_t camera_copy_latest_frame(camera_frame_buffer_t *out_frame) {
  return camera_copy_frame(out_frame, true);
}

esp_err_t camera_copy_current_frame(camera_frame_buffer_t *out_frame) {
  return camera_copy_frame(out_frame, false);
}

void Camera_Capture_Task(void *pvParameters) {
  (void)pvParameters;
  ESP_LOGI(TAG, "Camera capture task running on dedicated Core");

  for (;;) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
      ESP_LOGW(TAG, "Frame grab failed: fb == NULL");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (fb->format == PIXFORMAT_JPEG) {
      if (fb->len < 4 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
        ESP_LOGW(TAG, "Dropping corrupted JPEG (len: %u)", (unsigned)fb->len);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
    }

    if (fb->len > FIXED_FRAME_CAPACITY) {
      ESP_LOGW(TAG, "Frame too large (%u > %d), dropped",
               (unsigned)fb->len, FIXED_FRAME_CAPACITY);
      esp_camera_fb_return(fb);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    uint32_t payload_len = fb->len;
    frame_slot_t *slot = &s_slots[s_write_idx];
    memcpy(slot->data, fb->buf, payload_len);

    slot->header.buffer_len = payload_len;
    slot->header.width = fb->width ? fb->width : DEFAULT_FRAME_WIDTH;
    slot->header.height = fb->height ? fb->height : DEFAULT_FRAME_HEIGHT;
    slot->header.format = (fb->format == PIXFORMAT_JPEG) ? FRAME_FORMAT_JPEG : FRAME_FORMAT_RAW;
    slot->header.frame_id = ++s_frame_counter;
    slot->header.timestamp_ms = esp_timer_get_time() / 1000;

    esp_camera_fb_return(fb);

    if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      int temp = s_ready_idx;
      s_ready_idx = s_write_idx;
      s_write_idx = temp;
      s_has_new_frame = true;
      xSemaphoreGive(s_camera_mutex);

      atomic_store(&s_last_frame_timestamp_ms,
                   (uint_fast64_t)(esp_timer_get_time() / 1000));
      atomic_store(&s_is_streaming, true);

      wifi_notify_stream_frame_ready();
    } else {
      ESP_LOGD(TAG, "Swap skipped, consumer busy — frame dropped");
    }

    taskYIELD();
  }
}

#define CAM_TASK_STACK_SIZE 4096
static StaticTask_t s_cam_task_buffer;
static StackType_t s_cam_task_stack[CAM_TASK_STACK_SIZE];

TaskHandle_t camera_task_start(UBaseType_t priority, BaseType_t core_id) {
  if (!s_is_initialized) {
    if (camera_task_init() != ESP_OK) return NULL;
  }
  return xTaskCreateStaticPinnedToCore(
      Camera_Capture_Task, "Cam_Capture_Task", CAM_TASK_STACK_SIZE, NULL,
      priority, s_cam_task_stack, &s_cam_task_buffer, core_id);
}
