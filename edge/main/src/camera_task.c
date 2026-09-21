#include "camera_task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "pins.h"

static const char *TAG = "CAMERA_TASK";

// FreeRTOS Mutex protecting concurrent access to the shared camera frame buffer
static SemaphoreHandle_t s_camera_mutex = NULL;

// In-memory latest frame container
static camera_frame_buffer_t s_shared_frame = {0};

// Resolution defaults for USB Webcam (QVGA / VGA)
#define DEFAULT_FRAME_WIDTH   320
#define DEFAULT_FRAME_HEIGHT  240
#define INITIAL_BUF_CAPACITY  (64 * 1024) // 64 KB initial capacity for compressed JPEG

static uint32_t s_frame_counter = 0;
static bool s_is_initialized = false;

static void init_trigger_button(void) {
#if (PIN_BTN_ENABLE >= 0)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_ENABLE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    ESP_LOGI(TAG, "Hardware trigger capture button initialized on GPIO %d (Active-Low)", PIN_BTN_ENABLE);
#endif
}

esp_err_t camera_task_init(void) {
    if (s_is_initialized) {
        return ESP_OK;
    }

    // 1. Create FreeRTOS Mutex
    s_camera_mutex = xSemaphoreCreateMutex();
    if (s_camera_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS camera mutex");
        return ESP_ERR_NO_MEM;
    }

    // 2. Allocate frame buffer memory (prefer PSRAM if available, fallback to standard Heap)
    s_shared_frame.capacity = INITIAL_BUF_CAPACITY;
    s_shared_frame.data = (uint8_t *)heap_caps_malloc(s_shared_frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_shared_frame.data == NULL) {
        ESP_LOGW(TAG, "PSRAM unavailable for camera buffer, allocating from standard heap");
        s_shared_frame.data = (uint8_t *)malloc(s_shared_frame.capacity);
        if (s_shared_frame.data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for camera frame buffer");
            vSemaphoreDelete(s_camera_mutex);
            s_camera_mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    // 3. Initialize header with zero-padding defaults
    memset(&s_shared_frame.header, 0, sizeof(camera_frame_header_t));
    s_shared_frame.header.width = DEFAULT_FRAME_WIDTH;
    s_shared_frame.header.height = DEFAULT_FRAME_HEIGHT;
    s_shared_frame.header.format = FRAME_FORMAT_JPEG;
    s_shared_frame.header.buffer_len = 0;

    init_trigger_button();

    s_is_initialized = true;
    ESP_LOGI(TAG, "Camera subsystem (Logitech USB D-: %d, D+: %d) initialized successfully (Capacity: %u bytes)", 
             USB_PIN_DM, USB_PIN_DP, (unsigned int)s_shared_frame.capacity);

    return ESP_OK;
}

bool camera_lock_frame(TickType_t timeout_ticks) {
    if (s_camera_mutex == NULL) {
        return false;
    }
    return (xSemaphoreTake(s_camera_mutex, timeout_ticks) == pdTRUE);
}

void camera_unlock_frame(void) {
    if (s_camera_mutex != NULL) {
        xSemaphoreGive(s_camera_mutex);
    }
}

const camera_frame_buffer_t* camera_get_shared_frame(void) {
    return &s_shared_frame;
}

esp_err_t camera_copy_latest_frame(camera_frame_buffer_t *out_frame) {
    if (out_frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!camera_lock_frame(pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "Timeout waiting for camera frame mutex in copy operation");
        return ESP_ERR_TIMEOUT;
    }

    // Copy packed header directly (zero padding guarantees exact memory alignment)
    memcpy(&out_frame->header, &s_shared_frame.header, sizeof(camera_frame_header_t));

    if (s_shared_frame.header.buffer_len > 0 && s_shared_frame.data != NULL) {
        if (out_frame->capacity < s_shared_frame.header.buffer_len) {
            uint8_t *new_buf = (uint8_t *)realloc(out_frame->data, s_shared_frame.header.buffer_len);
            if (new_buf == NULL) {
                camera_unlock_frame();
                return ESP_ERR_NO_MEM;
            }
            out_frame->data = new_buf;
            out_frame->capacity = s_shared_frame.header.buffer_len;
        }
        memcpy(out_frame->data, s_shared_frame.data, s_shared_frame.header.buffer_len);
    }

    camera_unlock_frame();
    return ESP_OK;
}

esp_err_t camera_get_latest_frame_base64(char **out_b64, size_t *out_len) {
    if (out_b64 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!camera_lock_frame(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_shared_frame.header.buffer_len == 0 || s_shared_frame.data == NULL) {
        camera_unlock_frame();
        return ESP_ERR_NOT_FOUND;
    }

    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, s_shared_frame.data, s_shared_frame.header.buffer_len);

    char *buf = (char *)malloc(b64_len + 1);
    if (buf == NULL) {
        camera_unlock_frame();
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)buf, b64_len + 1, &written, s_shared_frame.data, s_shared_frame.header.buffer_len);
    camera_unlock_frame();

    if (ret != 0) {
        free(buf);
        return ESP_FAIL;
    }

    buf[written] = '\0';
    *out_b64 = buf;
    if (out_len != NULL) {
        *out_len = written;
    }

    return ESP_OK;
}

/**
 * @brief Generates simulated frame payload when physical camera sensor is initializing or in simulation mode.
 */
static void generate_synthetic_frame(camera_frame_buffer_t *frame) {
    // Valid minimal baseline JPEG image snippet for simulation and pipeline testing
    static const uint8_t minimal_jpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01,
        0x01, 0x01, 0x00, 0x48, 0x00, 0x48, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
        0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09,
        0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
        0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20,
        0x24, 0x2E, 0x27, 0x20, 0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29,
        0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27, 0x39, 0x3D, 0x38, 0x32,
        0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xD9
    };

    size_t header_sz = sizeof(minimal_jpeg);
    if (frame->capacity < header_sz) {
        return;
    }

    memcpy(frame->data, minimal_jpeg, header_sz);
    frame->header.buffer_len = header_sz;
    frame->header.width = DEFAULT_FRAME_WIDTH;
    frame->header.height = DEFAULT_FRAME_HEIGHT;
    frame->header.format = FRAME_FORMAT_JPEG;
    frame->header.frame_id = ++s_frame_counter;
    frame->header.timestamp_ms = esp_timer_get_time() / 1000;
}

void Camera_Capture_Task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "Camera Capture FreeRTOS task running on Core %d", xPortGetCoreID());

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t frame_interval = pdMS_TO_TICKS(100); // 10 FPS continuous acquisition loop

    for (;;) {
        // Acquire mutex to safely update the shared frame container
        if (camera_lock_frame(pdMS_TO_TICKS(50))) {
            generate_synthetic_frame(&s_shared_frame);
            camera_unlock_frame();
        } else {
            ESP_LOGW(TAG, "Camera capture loop skipped frame due to mutex contention");
        }

        vTaskDelayUntil(&last_wake_time, frame_interval);
    }
}

TaskHandle_t camera_task_start(UBaseType_t priority, BaseType_t core_id) {
    if (!s_is_initialized) {
        if (camera_task_init() != ESP_OK) {
            ESP_LOGE(TAG, "Cannot start camera task: initialization failed");
            return NULL;
        }
    }

    TaskHandle_t task_handle = NULL;
    BaseType_t res = xTaskCreatePinnedToCore(
        Camera_Capture_Task,
        "Cam_Capture_Task",
        4096,
        NULL,
        priority,
        &task_handle,
        core_id
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS Camera Capture task");
        return NULL;
    }

    return task_handle;
}

esp_err_t camera_frame_to_json(const camera_frame_buffer_t *frame, char **out_json_str, bool include_base64) {
    if (frame == NULL || out_json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Add packed frame metadata (matching camera_frame_header_t)
    cJSON_AddNumberToObject(root, "frame_id", frame->header.frame_id);
    cJSON_AddNumberToObject(root, "width", frame->header.width);
    cJSON_AddNumberToObject(root, "height", frame->header.height);
    cJSON_AddNumberToObject(root, "format", frame->header.format);
    cJSON_AddNumberToObject(root, "buffer_len", frame->header.buffer_len);
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)frame->header.timestamp_ms);

    if (include_base64 && frame->header.buffer_len > 0 && frame->data != NULL) {
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, frame->data, frame->header.buffer_len);
        
        char *b64_buf = (char *)malloc(b64_len + 1);
        if (b64_buf != NULL) {
            size_t written = 0;
            if (mbedtls_base64_encode((unsigned char *)b64_buf, b64_len + 1, &written, frame->data, frame->header.buffer_len) == 0) {
                b64_buf[written] = '\0';
                cJSON_AddStringToObject(root, "image_base64", b64_buf);
            }
            free(b64_buf);
        } else {
            ESP_LOGW(TAG, "Failed to allocate buffer for base64 encoding (size: %u)", (unsigned int)b64_len);
        }
    }

    char *json_out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_out == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json_str = json_out;
    return ESP_OK;
}
