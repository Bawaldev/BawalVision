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

static const char *TAG = "CAMERA_OV5640";

#define FIXED_FRAME_CAPACITY INITIAL_BUF_CAPACITY /* 64 KB or 128 KB */

typedef struct {
  camera_frame_header_t header;
  uint8_t *data;
} frame_slot_t;

/* Double-buffering slots */
static frame_slot_t s_slots[2] = {0};
static volatile int s_ready_idx = 0;       /* Buffer siap kirim untuk HTTP */
static volatile int s_write_idx = 1;       /* Buffer kerja untuk penangkap kamera */
static volatile bool s_has_new_frame = false;

static SemaphoreHandle_t s_camera_mutex = NULL;
static uint32_t s_frame_counter = 0;
static bool s_is_initialized = false;
static atomic_bool s_is_streaming = false;
static char s_detected_camera_model[32] = "OV5640";

bool is_camera_streaming(void) { return atomic_load(&s_is_streaming); }

const char *camera_get_sensor_name(void) {
  return s_detected_camera_model;
}

esp_err_t camera_task_init(void) {
  if (s_is_initialized)
    return ESP_OK;

  s_camera_mutex = xSemaphoreCreateMutex();
  if (s_camera_mutex == NULL)
    return ESP_ERR_NO_MEM;

  /* Pre-alokasi 2 slot buffer tetap di PSRAM sekali saat boot */
  for (int i = 0; i < 2; i++) {
    s_slots[i].data = (uint8_t *)heap_caps_malloc(
        FIXED_FRAME_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slots[i].data == NULL) {
      s_slots[i].data = (uint8_t *)malloc(FIXED_FRAME_CAPACITY);
    }
    if (s_slots[i].data == NULL) {
      vSemaphoreDelete(s_camera_mutex);
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

      .xclk_freq_hz = 20000000,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,

      .pixel_format = PIXFORMAT_JPEG,
      .frame_size = FRAMESIZE_QVGA, // 320x240 (anti-bufferbloat target)
      .jpeg_quality = 26,           // Stable 4-6 KB payload, prevents VBR motion spikes
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
      snprintf(s_detected_camera_model, sizeof(s_detected_camera_model), "SENSOR_0x%04X", (unsigned int)s->id.PID);
    }
    ESP_LOGI(TAG, "Camera sensor scanned successfully: %s (PID: 0x%04x)", s_detected_camera_model, (unsigned int)s->id.PID);

    // =========================================================================
    // 1. FRAME GEOMETRY & JPEG COMPRESSION
    // =========================================================================
    s->set_vflip(s, 1);                  // Kunci vertikal (tegak)
    s->set_hmirror(s, 0);                // Kunci horizontal (tanpa mirror)
    s->set_framesize(s, FRAMESIZE_QVGA); // 320x240 resolution
    s->set_quality(s, 26);               // Quality 26: 4-6 KB stable payload anti-VBR spike

    // =========================================================================
    // 2. AUTOMATIC WHITE BALANCE (AWB) ENGINE
    // =========================================================================
    s->set_whitebal(s, 1);              // Enable hardware Auto White Balance (0 = disable, 1 = enable)
    s->set_awb_gain(s, 1);              // Enable AWB gain multipliers across R, G, and B color channels
    s->set_wb_mode(s, 0);               // AWB operating mode: 0 = Auto / Neutral ambient tracking

    // =========================================================================
    // 3. EXPOSURE & ANALOG GAIN CONTROL (ANTI-HOTSPOT & ANTI-BLACKOUT)
    // =========================================================================
    s->set_exposure_ctrl(s, 1);         // Enable hardware Automatic Exposure Control (AEC)
    s->set_aec2(s, 0);                  // Disable DSP AEC2 (prevent indoor framerate degradation)
    s->set_ae_level(s, 0);              // Target exposure compensation level: 0 = Standard/Balanced
    s->set_gain_ctrl(s, 1);             // Enable Automatic Gain Control (AGC) circuit
    s->set_gainceiling(s, (gainceiling_t)GAINCEILING_8X); // 8x analog gain ceiling prevents high ISO noise grain

    // =========================================================================
    // 4. HARDWARE OPTICAL & LENS CALIBRATION
    // =========================================================================
    s->set_bpc(s, 1);                   // Black Pixel Correction (1 = enabled to eliminate thermal sensor dead pixels)
    s->set_wpc(s, 1);                   // White Pixel Correction (1 = enabled to filter out hot/stuck pixels from thermal drift)
    s->set_raw_gma(s, 1);               // Enable sensor raw Gamma correction curve to preserve midtone dynamic range
    s->set_lenc(s, 1);                  // Enable Lens Vignetting Correction (equalizes illumination across optical corners)
    s->set_dcw(s, 0);                   // Downsize / Crop Windowing disabled (preserves standard optical FOV)

    // =========================================================================
    // 5. COLOR & SPATIAL TUNING (ENHANCED DETAIL FOR COMPUTER VISION)
    // =========================================================================
    s->set_brightness(s, 0);            // Sensor baseline brightness: 0 = Neutral calibrated level
    s->set_contrast(s, 1);              // Contrast +1: Enhances separation between food objects and background
    s->set_saturation(s, 1);            // Saturation +1: Vivid color discrimination for freshness classification
    s->set_sharpness(s, 1);             // Edge sharpness +1: Crisper boundary delineation without ringing
    if (s->set_denoise) {
      s->set_denoise(s, 2);             // Hardware 2D spatial de-noising level 2 (suppresses CMOS sensor grain)
    }
  }

  s_is_initialized = true;
  ESP_LOGI(TAG, "Pre-allocated 2x %d KB PSRAM frame buffers OK", FIXED_FRAME_CAPACITY / 1024);
  return ESP_OK;
}

/* Mengambil salinan frame untuk HTTP sender dengan durasi lock seminimal mungkin */
esp_err_t camera_copy_latest_frame(camera_frame_buffer_t *out_frame) {
  if (out_frame == NULL || out_frame->data == NULL)
    return ESP_ERR_INVALID_ARG;

  /* Timeout 25ms: cukup untuk menunggu giliran swap tanpa menahan pipeline */
  if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(25)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  frame_slot_t *slot = &s_slots[s_ready_idx];

  /* Hanya kirim frame riil dari sensor kamera */
  if (!s_has_new_frame || slot->header.buffer_len == 0) {
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
  s_has_new_frame = false;

  xSemaphoreGive(s_camera_mutex);
  return ESP_OK;
}

void Camera_Capture_Task(void *pvParameters) {
  (void)pvParameters;
  ESP_LOGI(TAG, "OV5640 CSP Frame Ingestion Task running");

  const TickType_t frame_interval_ticks = pdMS_TO_TICKS(1000 / STREAM_FPS);

  for (;;) {
    TickType_t start_tick = xTaskGetTickCount();

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
      ESP_LOGW(TAG, "Frame grab failed: esp_camera_fb_get returned NULL");
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    /* Validasi penanda JPEG SOI (0xFF, 0xD8) */
    if (fb->format == PIXFORMAT_JPEG) {
      if (fb->len < 4 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
        ESP_LOGW(TAG, "Dropping corrupted JPEG frame (len: %u)", (unsigned)fb->len);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }

    atomic_store(&s_is_streaming, true);

    /* Clamping ukuran frame tanpa realloc dinamis */
    uint32_t payload_len = fb->len;
    if (payload_len > FIXED_FRAME_CAPACITY) {
      ESP_LOGW(TAG, "Frame melebihi alokasi (%u > %d), clamped", (unsigned)payload_len, FIXED_FRAME_CAPACITY);
      payload_len = FIXED_FRAME_CAPACITY;
    }

    /* Tulis frame ke slot kerja saat ini */
    frame_slot_t *slot = &s_slots[s_write_idx];
    memcpy(slot->data, fb->buf, payload_len);

    slot->header.buffer_len = payload_len;
    slot->header.width = fb->width ? fb->width : DEFAULT_FRAME_WIDTH;
    slot->header.height = fb->height ? fb->height : DEFAULT_FRAME_HEIGHT;
    slot->header.format = (fb->format == PIXFORMAT_JPEG) ? FRAME_FORMAT_JPEG : FRAME_FORMAT_RAW;
    slot->header.frame_id = ++s_frame_counter;
    slot->header.timestamp_ms = esp_timer_get_time() / 1000;

    /* Kembalikan buffer hardware DMA DVP secepat mungkin */
    esp_camera_fb_return(fb);

    /* Pointer Swap instan: tunggu mutex hingga 10ms jika consumer sedang menyalin frame */
    if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      int temp = s_ready_idx;
      s_ready_idx = s_write_idx;
      s_write_idx = temp;
      s_has_new_frame = true;
      xSemaphoreGive(s_camera_mutex);
      wifi_notify_stream_frame_ready();
    } else {
      /* Jika consumer sedang membaca terlalu lama, frame berikutnya akan menimpa slot kerja */
      ESP_LOGD(TAG, "HTTP task sedang membaca, frame ditimpa tanpa blocking");
    }

    TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
    if (elapsed_ticks < frame_interval_ticks) {
      vTaskDelay(frame_interval_ticks - elapsed_ticks);
    } else {
      taskYIELD();
    }
  }
}

#define CAM_TASK_STACK_SIZE 4096
static StaticTask_t s_cam_task_buffer;
static StackType_t s_cam_task_stack[CAM_TASK_STACK_SIZE];

TaskHandle_t camera_task_start(UBaseType_t priority, BaseType_t core_id) {
  if (!s_is_initialized) {
    if (camera_task_init() != ESP_OK)
      return NULL;
  }

  return xTaskCreateStaticPinnedToCore(
      Camera_Capture_Task, "Cam_Capture_Task", CAM_TASK_STACK_SIZE, NULL,
      priority, s_cam_task_stack, &s_cam_task_buffer, core_id);
}