#include "camera_relay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "camera_task.h"
#include "data.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "system_monitor.h"
#include "wifi.h"

static const char *TAG = "CAM_RELAY";

#ifndef CONFIG_CAMERA_RELAY_URL
#define CONFIG_CAMERA_RELAY_URL ""
#endif

#ifndef CONFIG_CAMERA_RELAY_KEY
#define CONFIG_CAMERA_RELAY_KEY ""
#endif

#ifndef CONFIG_CAMERA_RELAY_FPS
#define CONFIG_CAMERA_RELAY_FPS RELAY_DEFAULT_FPS
#endif

#ifndef CONFIG_CAMERA_RELAY_DEVICE
#define CONFIG_CAMERA_RELAY_DEVICE "default"
#endif

#define RELAY_TASK_STACK 4096
#define RELAY_PUMP_MS 2000
#define RELAY_SEND_TIMEOUT_MS 5000
#define RELAY_TELEMETRY_MS 3000
#define RELAY_MIN_VALID_TIME 1700000000LL
#define RELAY_SNTP_RESTART_MS 30000

static char s_url[RELAY_URL_MAX];
static char s_key[RELAY_KEY_MAX];
static int s_fps = RELAY_DEFAULT_FPS;
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_cfg_version;
static volatile uint32_t s_cfg_seen;
static volatile bool s_connected;
static volatile uint32_t s_frames_sent;
static volatile uint32_t s_send_errors;
static volatile int64_t s_last_frame_ms;
static volatile uint32_t s_last_sent_id;
static volatile uint32_t s_last_sent_ms;
static volatile uint32_t s_last_telemetry_ms;
static esp_websocket_client_handle_t s_client;
static camera_frame_buffer_t s_frame;
static bool s_started;
static bool s_time_wait_logged;
static int64_t s_time_wait_started_ms;
static int64_t s_time_last_log_ms;

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void load_config(void) {
    char url[RELAY_URL_MAX] = {0};
    char key[RELAY_KEY_MAX] = {0};
    int fps = RELAY_DEFAULT_FPS;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(url);
        (void)nvs_get_str(handle, NVS_KEY_RELAY_URL, url, &len);
        len = sizeof(key);
        (void)nvs_get_str(handle, NVS_KEY_RELAY_KEY, key, &len);
        int32_t stored_fps = 0;
        if (nvs_get_i32(handle, NVS_KEY_RELAY_FPS, &stored_fps) == ESP_OK) {
            fps = (int)stored_fps;
        }
        nvs_close(handle);
    }

    if (url[0] == '\0') copy_str(url, sizeof(url), CONFIG_CAMERA_RELAY_URL);
    if (key[0] == '\0') copy_str(key, sizeof(key), CONFIG_CAMERA_RELAY_KEY);
    if (fps <= 0 || fps > RELAY_MAX_FPS) fps = CONFIG_CAMERA_RELAY_FPS;
    if (fps <= 0 || fps > RELAY_MAX_FPS) fps = RELAY_DEFAULT_FPS;

    taskENTER_CRITICAL(&s_cfg_mux);
    copy_str(s_url, sizeof(s_url), url);
    copy_str(s_key, sizeof(s_key), key);
    s_fps = fps;
    taskEXIT_CRITICAL(&s_cfg_mux);
}

static uint32_t snapshot_config(char *url, size_t url_len,
                                char *key, size_t key_len, int *fps) {
    taskENTER_CRITICAL(&s_cfg_mux);
    copy_str(url, url_len, s_url);
    copy_str(key, key_len, s_key);
    if (fps) *fps = s_fps;
    uint32_t version = s_cfg_version;
    taskEXIT_CRITICAL(&s_cfg_mux);
    return version;
}

static void normalize_base(const char *base, char *out, size_t out_len) {
    if (strncmp(base, "https://", 8) == 0) {
        snprintf(out, out_len, "wss://%s", base + 8);
    } else if (strncmp(base, "http://", 7) == 0) {
        snprintf(out, out_len, "ws://%s", base + 7);
    } else {
        copy_str(out, out_len, base);
    }
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
}

static void append_ingest_path(const char *base, char *out, size_t out_len) {
    const char *scheme = strstr(base, "://");
    const char *authority = scheme ? scheme + 3 : base;
    const char *slash = strchr(authority, '/');
    if (slash && slash[1] != '\0') {
        copy_str(out, out_len, base);
        return;
    }
    size_t prefix = slash ? (size_t)(slash - base) : strlen(base);
    snprintf(out, out_len, "%.*s/ingest", (int)prefix, base);
}

static bool build_ingest_uri(const char *base, const char *device,
                             const char *key, char *out, size_t out_len) {
    char normalized[RELAY_URL_MAX];
    char path[RELAY_URL_MAX];
    normalize_base(base, normalized, sizeof(normalized));
    if (strncmp(normalized, "ws://", 5) != 0 &&
        strncmp(normalized, "wss://", 6) != 0) {
        return false;
    }
    append_ingest_path(normalized, path, sizeof(path));
    char sep = strchr(path, '?') ? '&' : '?';
    if (key && key[0] != '\0') {
        snprintf(out, out_len, "%s%cdevice=%s&key=%s", path, sep, device, key);
    } else {
        snprintf(out, out_len, "%s%cdevice=%s", path, sep, device);
    }
    return out[0] != '\0';
}

static void relay_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    (void)arg;
    (void)base;
    (void)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_connected = true;
            s_send_errors = 0;
            s_time_wait_logged = false;
            s_last_sent_id = 0;
            s_last_sent_ms = 0;
            s_last_telemetry_ms = 0;
            s_time_wait_started_ms = 0;
            s_time_last_log_ms = 0;
            ESP_LOGI(TAG, "Connected to camera relay");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "Disconnected from camera relay");
            break;
        case WEBSOCKET_EVENT_ERROR: {
            s_connected = false;
            s_send_errors++;
            esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
            if (data) {
                ESP_LOGW(TAG, "Camera relay websocket error type=%d tls=%d sock_errno=%d",
                         (int)data->error_handle.error_type,
                         data->error_handle.esp_tls_last_esp_err,
                         data->error_handle.esp_transport_sock_errno);
            } else {
                ESP_LOGW(TAG, "Camera relay websocket error");
            }
            break;
        }
        case WEBSOCKET_EVENT_CLOSED: {
            s_connected = false;
            esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
            ESP_LOGW(TAG, "Camera relay closed code=%d",
                     data ? data->close_status_code : -1);
            break;
        }
        default:
            break;
    }
}

static void stop_client(void) {
    if (s_client) {
        (void)esp_websocket_client_stop(s_client);
        (void)esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
}

static bool start_client(const char *uri) {
    esp_websocket_client_config_t cfg = {0};
    cfg.uri = uri;
    cfg.reconnect_timeout_ms = 3000;
    cfg.network_timeout_ms = 8000;
    cfg.buffer_size = 4096;
    cfg.task_stack = 8192;
    cfg.ping_interval_sec = 3600;
    cfg.pingpong_timeout_sec = 0;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_websocket_register_events(
        client, WEBSOCKET_EVENT_ANY, relay_event_handler, NULL);
    if (err != ESP_OK) {
        (void)esp_websocket_client_destroy(client);
        return false;
    }

    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        (void)esp_websocket_client_destroy(client);
        return false;
    }

    s_client = client;
    return true;
}

static void send_telemetry_over_relay(void) {
    if (!s_client || !esp_websocket_client_is_connected(s_client)) return;

    telemetry_data_t meta;
    init_device_metadata(&meta);

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    const float temperature = get_esp32_cpu_temperature();
    cJSON_AddStringToObject(root, "type", "telemetry");
    cJSON_AddStringToObject(root, "device_id", meta.device_id);
    cJSON_AddStringToObject(root, "firmware_version", meta.firmware_version);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "temperature", (double)temperature);
    cJSON_AddNumberToObject(root, "cpu_temperature", (double)temperature);
    cJSON_AddNumberToObject(root, "cpu_load", (double)get_cpu_usage_percentage());
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_heap_internal",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_heap_psram",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(root, "camera_model", camera_get_sensor_name());

    char ip[24] = {0};
    if (wifi_get_sta_ip(ip, sizeof(ip))) {
        cJSON_AddStringToObject(root, "ip_address", ip);
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    int sent = esp_websocket_client_send_text(
        s_client, json, (int)strlen(json), pdMS_TO_TICKS(1000));
    if (sent <= 0) {
        s_send_errors++;
    }
    cJSON_free(json);
}

static void pump_frames(int fps) {
    if (!s_frame.data) return;
    if (fps <= 0 || fps > RELAY_MAX_FPS) fps = RELAY_DEFAULT_FPS;

    uint32_t interval_ms = 1000U / (uint32_t)fps;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(RELAY_PUMP_MS);

    while (xTaskGetTickCount() < until) {
        if (!s_client || !esp_websocket_client_is_connected(s_client)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_last_telemetry_ms == 0 ||
            now_ms - s_last_telemetry_ms >= RELAY_TELEMETRY_MS) {
            send_telemetry_over_relay();
            s_last_telemetry_ms = now_ms;
        }

        camera_frame_buffer_t frame = s_frame;
        if (camera_copy_current_frame(&frame) == ESP_OK &&
            frame.header.buffer_len > 0) {
            bool due = (s_last_sent_ms == 0) || (now_ms - s_last_sent_ms >= interval_ms);
            if (frame.header.frame_id != s_last_sent_id && due) {
                int sent = esp_websocket_client_send_bin(
                    s_client, (const char *)frame.data,
                    (int)frame.header.buffer_len,
                    pdMS_TO_TICKS(RELAY_SEND_TIMEOUT_MS));
                if (sent <= 0) {
                    s_send_errors++;
                    if (s_send_errors <= 5) {
                        ESP_LOGW(TAG, "Relay send failed (%d)", sent);
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                    break;
                }
                s_last_sent_id = frame.header.frame_id;
                s_last_sent_ms = now_ms;
                s_last_frame_ms = (int64_t)now_ms;
                s_frames_sent++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static bool relay_needs_valid_time(const char *url) {
    return strncmp(url, "wss://", 6) == 0 || strncmp(url, "https://", 8) == 0;
}

static void relay_task(void *arg) {
    (void)arg;

    s_frame.capacity = INITIAL_BUF_CAPACITY;
    s_frame.data = (uint8_t *)heap_caps_malloc(
        s_frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame.data) {
        s_frame.data = (uint8_t *)malloc(s_frame.capacity);
    }
    if (!s_frame.data) {
        ESP_LOGE(TAG, "Failed to allocate relay frame buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        char url[RELAY_URL_MAX] = {0};
        char key[RELAY_KEY_MAX] = {0};
        int fps = RELAY_DEFAULT_FPS;
        uint32_t version = snapshot_config(url, sizeof(url), key, sizeof(key), &fps);

        if (version != s_cfg_seen) {
            stop_client();
            s_cfg_seen = version;
        }

        if (url[0] == '\0') {
            stop_client();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!g_wifi_event_group ||
            !(xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            stop_client();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char uri[RELAY_URL_MAX + RELAY_KEY_MAX + 64];
        if (!build_ingest_uri(url, CONFIG_CAMERA_RELAY_DEVICE, key, uri, sizeof(uri))) {
            ESP_LOGE(TAG, "Invalid camera relay URL");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!s_client) {
            if (relay_needs_valid_time(uri) && time(NULL) < RELAY_MIN_VALID_TIME) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (s_time_wait_started_ms == 0) {
                    s_time_wait_started_ms = now_ms;
                }
                if (!s_time_wait_logged ||
                    now_ms - s_time_last_log_ms >= RELAY_SNTP_RESTART_MS) {
                    ESP_LOGW(TAG,
                             "Waiting for SNTP time before WSS handshake (%.0fs)",
                             (double)(now_ms - s_time_wait_started_ms) / 1000.0);
                    s_time_wait_logged = true;
                    s_time_last_log_ms = now_ms;
                }
                if (now_ms - s_time_wait_started_ms >= RELAY_SNTP_RESTART_MS) {
                    wifi_sntp_restart();
                    s_time_wait_started_ms = now_ms;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            s_time_wait_started_ms = 0;
            s_time_last_log_ms = 0;
            if (!start_client(uri)) {
                ESP_LOGW(TAG, "Could not start camera relay client");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            ESP_LOGI(TAG, "Connecting to camera relay %s (stream %s)",
                     url, CONFIG_CAMERA_RELAY_DEVICE);
        }

        pump_frames(fps);
    }
}

void camera_relay_start(void) {
    if (s_started) return;
    load_config();
    s_cfg_seen = s_cfg_version;
    s_started = true;

    BaseType_t created = xTaskCreatePinnedToCore(
        relay_task, "camera_relay", RELAY_TASK_STACK, NULL, 3, NULL, 0);
    if (created != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "Failed to start camera relay task");
    }
}

bool camera_relay_set_config(const char *url, const char *key, int fps) {
    if (!url) return false;
    if (strlen(url) >= RELAY_URL_MAX) return false;
    if (key && strlen(key) >= RELAY_KEY_MAX) return false;

    if (url[0] != '\0' &&
        strncmp(url, "ws://", 5) != 0 &&
        strncmp(url, "wss://", 6) != 0 &&
        strncmp(url, "http://", 7) != 0 &&
        strncmp(url, "https://", 8) != 0) {
        return false;
    }

    if (fps <= 0 || fps > RELAY_MAX_FPS) fps = RELAY_DEFAULT_FPS;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(handle, NVS_KEY_RELAY_URL, url);
    if (err == ESP_OK && key) err = nvs_set_str(handle, NVS_KEY_RELAY_KEY, key);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_RELAY_FPS, fps);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return false;

    taskENTER_CRITICAL(&s_cfg_mux);
    copy_str(s_url, sizeof(s_url), url);
    if (key) copy_str(s_key, sizeof(s_key), key);
    s_fps = fps;
    s_cfg_version++;
    taskEXIT_CRITICAL(&s_cfg_mux);
    return true;
}

bool camera_relay_get_config(char *url, size_t url_len,
                             char *key, size_t key_len,
                             int *fps) {
    taskENTER_CRITICAL(&s_cfg_mux);
    if (url && url_len) copy_str(url, url_len, s_url);
    if (key && key_len) copy_str(key, key_len, s_key);
    if (fps) *fps = s_fps;
    bool configured = s_url[0] != '\0';
    taskEXIT_CRITICAL(&s_cfg_mux);
    return configured;
}

bool camera_relay_is_connected(void) {
    return s_connected;
}

uint32_t camera_relay_frames_sent(void) {
    return s_frames_sent;
}

uint32_t camera_relay_send_errors(void) {
    return s_send_errors;
}

int64_t camera_relay_last_frame_ms(void) {
    return s_last_frame_ms;
}
