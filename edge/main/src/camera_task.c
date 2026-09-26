#include "camera_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pins.h"
#include "standby_jpeg.h"
#include "system_monitor.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "wifi.h"

static const char *TAG = "CAMERA_UVC";

static SemaphoreHandle_t s_camera_mutex = NULL;
static camera_frame_buffer_t s_shared_frame = {0};
static uvc_host_stream_hdl_t s_stream_hdl = NULL;

static uint32_t s_frame_counter = 0;
static bool s_is_initialized = false;
static bool s_is_streaming = false;

static bool camera_lock_frame(TickType_t timeout_ticks) {
  if (s_camera_mutex == NULL) return false;
  return (xSemaphoreTake(s_camera_mutex, timeout_ticks) == pdTRUE);
}

static void camera_unlock_frame(void) {
  if (s_camera_mutex != NULL) {
    xSemaphoreGive(s_camera_mutex);
  }
}

bool is_camera_streaming(void) {
  return s_is_streaming;
}

static bool camera_uvc_frame_cb(const uvc_host_frame_t *frame, void *user_ctx) {
  (void)user_ctx;
  if (frame == NULL || frame->data == NULL || frame->data_len == 0) return true;

  if (camera_lock_frame(pdMS_TO_TICKS(100))) {
    if (s_shared_frame.capacity < frame->data_len) {
      uint8_t *new_buf = (uint8_t *)heap_caps_realloc(
          s_shared_frame.data, frame->data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (new_buf != NULL) {
        s_shared_frame.data = new_buf;
        s_shared_frame.capacity = frame->data_len;
      } else {
        camera_unlock_frame();
        return true; 
      }
    }

    if (s_shared_frame.capacity >= frame->data_len && s_shared_frame.data != NULL) {
      memcpy(s_shared_frame.data, frame->data, frame->data_len);
      s_shared_frame.header.buffer_len = frame->data_len;
      s_shared_frame.header.width = frame->vs_format.h_res ? frame->vs_format.h_res : DEFAULT_FRAME_WIDTH;
      s_shared_frame.header.height = frame->vs_format.v_res ? frame->vs_format.v_res : DEFAULT_FRAME_HEIGHT;
      s_shared_frame.header.format = (frame->vs_format.format == UVC_VS_FORMAT_MJPEG) ? FRAME_FORMAT_JPEG : FRAME_FORMAT_RAW;
      s_shared_frame.header.frame_id = ++s_frame_counter;
      s_shared_frame.header.timestamp_ms = esp_timer_get_time() / 1000;
    }
    camera_unlock_frame();
  } else {
    ESP_LOGW(TAG, "UVC Callback dropped frame due to Mutex timeout!");
  }

  return true;
}

static void camera_uvc_stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx) {
  (void)user_ctx;
  if (event == NULL) return;
  if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
    ESP_LOGW(TAG, "Logitech UVC Camera Disconnected!");
    s_is_streaming = false;
  }
}

static void usb_lib_task(void *pvParameters) {
  (void)pvParameters;
  while (1) {
    uint32_t event_flags;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (err == ESP_OK) {
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
        ESP_LOGD(TAG, "No USB host clients registered");
      }
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
        break;
      }
    }
  }
  vTaskDelete(NULL);
}

esp_err_t camera_task_init(void) {
  if (s_is_initialized) return ESP_OK;

  s_camera_mutex = xSemaphoreCreateMutex();
  if (s_camera_mutex == NULL) return ESP_ERR_NO_MEM;

  s_shared_frame.capacity = INITIAL_BUF_CAPACITY;
  s_shared_frame.data = (uint8_t *)heap_caps_malloc(s_shared_frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_shared_frame.data == NULL) {
    vSemaphoreDelete(s_camera_mutex);
    return ESP_ERR_NO_MEM;
  }

  memset(&s_shared_frame.header, 0, sizeof(camera_frame_header_t));

  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  esp_err_t err_usb = usb_host_install(&host_config);
  ESP_LOGI(TAG, "USB Host Install Result: %s", esp_err_to_name(err_usb));
  if (err_usb != ESP_OK) {
    return err_usb;
  }

  /* Core 0 handles USB host low-level interrupts and event loop */
  BaseType_t task_created = xTaskCreatePinnedToCore(
      usb_lib_task, "usb_lib_task", 4096, NULL, 10, NULL, 0);
  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create USB lib background task");
    return ESP_FAIL;
  }

  uvc_host_driver_config_t driver_cfg = {
      .driver_task_stack_size = 4096,
      .driver_task_priority = 5,
      .xCoreID = 0,
      .create_background_task = true,
  };
  esp_err_t err_uvc = uvc_host_install(&driver_cfg);
  ESP_LOGI(TAG, "UVC Host Driver Install Result: %s", esp_err_to_name(err_uvc));
  if (err_uvc != ESP_OK) {
    return err_uvc;
  }

  s_is_initialized = true;
  return ESP_OK;
}

esp_err_t camera_copy_latest_frame(camera_frame_buffer_t *out_frame) {
  if (out_frame == NULL) return ESP_ERR_INVALID_ARG;

  if (!camera_lock_frame(pdMS_TO_TICKS(100))) {
    return ESP_ERR_TIMEOUT;
  }

  if (s_shared_frame.header.buffer_len > 0 && s_shared_frame.data != NULL) {
    memcpy(&out_frame->header, &s_shared_frame.header, sizeof(camera_frame_header_t));

    if (out_frame->capacity < s_shared_frame.header.buffer_len) {
      uint8_t *new_buf = (uint8_t *)heap_caps_realloc(
          out_frame->data, s_shared_frame.header.buffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (new_buf == NULL) {
        camera_unlock_frame();
        return ESP_ERR_NO_MEM;
      }
      out_frame->data = new_buf;
      out_frame->capacity = s_shared_frame.header.buffer_len;
    }
    memcpy(out_frame->data, s_shared_frame.data, s_shared_frame.header.buffer_len);
  } else {
    uint32_t fallback_len = sizeof(s_fallback_jpeg);

    if (out_frame->capacity < fallback_len) {
      uint8_t *new_buf = (uint8_t *)heap_caps_realloc(
          out_frame->data, fallback_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (new_buf == NULL) {
        camera_unlock_frame();
        return ESP_ERR_NO_MEM;
      }
      out_frame->data = new_buf;
      out_frame->capacity = fallback_len;
    }

    out_frame->header.buffer_len = fallback_len;
    out_frame->header.width = DEFAULT_FRAME_WIDTH;
    out_frame->header.height = DEFAULT_FRAME_HEIGHT;
    out_frame->header.format = FRAME_FORMAT_JPEG;
    out_frame->header.frame_id = ++s_frame_counter;
    out_frame->header.timestamp_ms = esp_timer_get_time() / 1000;

    if (out_frame->data != NULL) {
      memcpy(out_frame->data, s_fallback_jpeg, fallback_len);
    }
  }

  camera_unlock_frame();
  return ESP_OK;
}

static const camera_profile_t s_camera_profiles[] = {
    { .width = 320, .height = 240, .fps = 30.0f, .format = UVC_VS_FORMAT_MJPEG, .label = "320x240 MJPEG @ 30 FPS" },
    { .width = 640, .height = 480, .fps = 30.0f, .format = UVC_VS_FORMAT_MJPEG, .label = "640x480 MJPEG @ 30 FPS" },
};

void Camera_Capture_Task(void *pvParameters) {
  (void)pvParameters;

  const size_t num_profiles = sizeof(s_camera_profiles) / sizeof(s_camera_profiles[0]);

  for (;;) {
    if (!s_is_streaming && s_stream_hdl == NULL) {
      bool stream_opened = false;

      // Step 1: Iterate through target profiles and attempt to open matching stream
      for (size_t i = 0; i < num_profiles; ++i) {
        ESP_LOGI(TAG, "Attempting to open camera stream with profile: %s", s_camera_profiles[i].label);

        uvc_host_stream_config_t stream_cfg = {
            .event_cb = camera_uvc_stream_event_cb,
            .frame_cb = camera_uvc_frame_cb,
            .vs_format = {
                .h_res = s_camera_profiles[i].width,
                .v_res = s_camera_profiles[i].height,
                .fps = s_camera_profiles[i].fps,
                .format = s_camera_profiles[i].format,
            },
            .advanced = {
                .number_of_frame_buffers = 2,
                .frame_size = 256 * 1024,
                .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                .number_of_urbs = 3,
            },
        };

        esp_err_t open_res = uvc_host_stream_open(&stream_cfg, pdMS_TO_TICKS(1000), &s_stream_hdl);
        if (open_res == ESP_OK) {
          ESP_LOGI(TAG, "Logitech UVC Stream Opened Successfully with profile: %s", s_camera_profiles[i].label);
          stream_opened = true;
          break;
        } else {
          ESP_LOGW(TAG, "Profile [%s] rejected (%s), trying next...",
                   s_camera_profiles[i].label, esp_err_to_name(open_res));
        }
      }

      // Step 2: Fallback to camera default format if no custom profiles could be opened
      if (!stream_opened) {
        ESP_LOGW(TAG, "All configured profiles failed. Attempting fallback to camera default format...");

        uvc_host_stream_config_t default_cfg = {
            .event_cb = camera_uvc_stream_event_cb,
            .frame_cb = camera_uvc_frame_cb,
            .vs_format = {
                .h_res = 0,
                .v_res = 0,
                .fps = 0,
                .format = UVC_VS_FORMAT_DEFAULT,
            },
            .advanced = {
                .number_of_frame_buffers = 2,
                .frame_size = 256 * 1024,
                .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                .number_of_urbs = 3,
            },
        };

        esp_err_t def_res = uvc_host_stream_open(&default_cfg, pdMS_TO_TICKS(1000), &s_stream_hdl);
        if (def_res == ESP_OK) {
          ESP_LOGI(TAG, "Logitech UVC Stream Opened with default format.");
          stream_opened = true;
        } else {
          ESP_LOGE(TAG, "Failed to open UVC stream with default format: %s", esp_err_to_name(def_res));
        }
      }

      // Step 3: Start streaming if stream interface handle was established
      if (stream_opened && s_stream_hdl != NULL) {
        uvc_host_stream_format_t active_fmt = {0};
        uvc_host_stream_format_get(s_stream_hdl, &active_fmt);
        ESP_LOGI(TAG, "Active stream format: %ux%u @ %.1f FPS (format=%d)",
                 active_fmt.h_res, active_fmt.v_res, active_fmt.fps, active_fmt.format);

        if (uvc_host_stream_start(s_stream_hdl) == ESP_OK) {
          s_is_streaming = true;
          ESP_LOGI(TAG, "Logitech UVC Streaming Started Successfully!");
        } else {
          ESP_LOGE(TAG, "Failed to start UVC Stream");
          uvc_host_stream_close(s_stream_hdl);
          s_stream_hdl = NULL;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

TaskHandle_t camera_task_start(UBaseType_t priority, BaseType_t core_id) {
  if (!s_is_initialized) {
    if (camera_task_init() != ESP_OK) return NULL;
  }

  TaskHandle_t handle = NULL;
  xTaskCreatePinnedToCore(Camera_Capture_Task, "Cam_Capture_Task", 4096, NULL, priority, &handle, core_id);
  return handle;
}

void http_stream_sender_task(void *pvParameters) {
  (void)pvParameters;

  if (g_wifi_event_group != NULL) {
    xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
  }

  camera_frame_buffer_t local_frame = {0};
  local_frame.capacity = INITIAL_BUF_CAPACITY;
  local_frame.data = (uint8_t *)heap_caps_malloc(local_frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (local_frame.data == NULL) {
    local_frame.data = (uint8_t *)malloc(local_frame.capacity);
  }

  if (local_frame.data == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for local frame buffer");
    vTaskDelete(NULL);
    return;
  }

  char device_id[32] = {0};
  get_internal_device_id(device_id, sizeof(device_id));

  char backend_ip[64] = {0};
  char full_url[URL_BUFFER_SIZE] = {0};
  uint32_t last_sent_frame_id = 0;
  char frame_id_str[16];
  const TickType_t frame_interval_ticks = pdMS_TO_TICKS(1000 / STREAM_FPS);

  ESP_LOGI(TAG, "HTTP Stream sender task started for device: %s", device_id);

  for (;;) {
    TickType_t start_tick = xTaskGetTickCount();

    if (camera_copy_latest_frame(&local_frame) == ESP_OK) {
      if (local_frame.header.buffer_len > 0 && local_frame.header.frame_id != last_sent_frame_id) {
        last_sent_frame_id = local_frame.header.frame_id;

        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
          strncpy(backend_ip, CONFIG_BACKEND_IP, sizeof(backend_ip) - 1);
        }

        snprintf(full_url, sizeof(full_url), "http://%s:%s%s", backend_ip, CONFIG_BACKEND_PORT, CAMERA_FRAME_ENDPOINT);

        esp_http_client_config_t config = {
            .url = full_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 2000,
            .buffer_size_tx = 4096,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client != NULL) {
          snprintf(frame_id_str, sizeof(frame_id_str), "%lu", (unsigned long)local_frame.header.frame_id);
          esp_http_client_set_header(client, "Content-Type", "image/jpeg");
          esp_http_client_set_header(client, "X-Device-ID", device_id);
          esp_http_client_set_header(client, "X-Frame-ID", frame_id_str);
          esp_http_client_set_post_field(client, (const char *)local_frame.data, local_frame.header.buffer_len);

          esp_err_t err = esp_http_client_perform(client);
          if (err != ESP_OK) {
            ESP_LOGW(TAG, "Frame dispatch failed: %s", esp_err_to_name(err));
          }
          esp_http_client_cleanup(client);
        }
      }
    }

    TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
    if (elapsed_ticks < frame_interval_ticks) {
      vTaskDelay(frame_interval_ticks - elapsed_ticks);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  if (local_frame.data) free(local_frame.data);
  vTaskDelete(NULL);
}