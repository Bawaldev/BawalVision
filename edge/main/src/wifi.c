#include "wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "camera_task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_camera.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lcd_task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "system_monitor.h"

static const char *TAG = "WIFI";

#define STREAM_TX_BUFFER_SIZE 24576
// Aggressive send timeout: never let one stalled Wi-Fi packet freeze capture.
#define HTTP_TIMEOUT_MS 400
// Adaptive pacing window: send immediately when slower than this interval,
// otherwise yield the remaining time back to the scheduler.
#define STREAM_TARGET_INTERVAL_MS 50

EventGroupHandle_t g_wifi_event_group = NULL;
int g_wifi_retry_num = 0;
char g_wifi_ssid[32] = {0};
char g_wifi_pass[64] = {0};
char g_backend_ip[32] = {0};
char g_backend_port[8] = "8080";

static char s_discovered_ip[32] = {0};
static TaskHandle_t s_stream_task_handle = NULL;
static esp_http_client_handle_t s_stream_client = NULL;
static char s_stream_url[URL_BUFFER_SIZE] = {0};
static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_wifi_initialized = false;

void wifi_notify_stream_frame_ready(void) {
    if (s_stream_task_handle != NULL) {
        xTaskNotifyGive(s_stream_task_handle);
    }
}

static void str_trim_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' || src[len - 1] == '\n')) {
        len--;
    }
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool is_valid_ipv4(const char *ip) {
    if (!ip || strlen(ip) < 7 || strlen(ip) > 15) return false;
    if (strcmp(ip, "0.0.0.0") == 0 || strcmp(ip, "255.255.255.255") == 0) return false;
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1 && addr.s_addr != 0;
}

bool wifi_nvs_load_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Fallback to legacy namespace if bawal_cfg does not exist yet
        err = nvs_open("wifi_config", NVS_READONLY, &handle);
        if (err != ESP_OK) return false;
    }

    size_t sz = sizeof(g_wifi_ssid);
    if (nvs_get_str(handle, NVS_KEY_WIFI_SSID, g_wifi_ssid, &sz) != ESP_OK) {
        sz = sizeof(g_wifi_ssid);
        (void)nvs_get_str(handle, "ssid", g_wifi_ssid, &sz);
    }

    sz = sizeof(g_wifi_pass);
    if (nvs_get_str(handle, NVS_KEY_WIFI_PASS, g_wifi_pass, &sz) != ESP_OK) {
        sz = sizeof(g_wifi_pass);
        (void)nvs_get_str(handle, "pass", g_wifi_pass, &sz);
    }

    sz = sizeof(g_backend_ip);
    (void)nvs_get_str(handle, NVS_KEY_BACKEND_IP, g_backend_ip, &sz);

    // Try reading backend_port as uint16 first, fallback to string
    uint16_t port_u16 = 8080;
    if (nvs_get_u16(handle, NVS_KEY_BACKEND_PORT, &port_u16) == ESP_OK && port_u16 > 0) {
        snprintf(g_backend_port, sizeof(g_backend_port), "%u", port_u16);
    } else {
        sz = sizeof(g_backend_port);
        if (nvs_get_str(handle, "backend_port", g_backend_port, &sz) != ESP_OK) {
            strncpy(g_backend_port, "8080", sizeof(g_backend_port) - 1);
        }
    }

    // Cross-namespace migration fallback: if bawal_cfg exists but was written
    // without a new password, keep using the legacy wifi_config values.
    if (g_wifi_pass[0] == '\0' || g_wifi_ssid[0] == '\0' || g_backend_ip[0] == '\0') {
        nvs_handle_t legacy;
        if (nvs_open("wifi_config", NVS_READONLY, &legacy) == ESP_OK) {
            if (g_wifi_ssid[0] == '\0') {
                sz = sizeof(g_wifi_ssid);
                (void)nvs_get_str(legacy, "ssid", g_wifi_ssid, &sz);
            }
            if (g_wifi_pass[0] == '\0') {
                sz = sizeof(g_wifi_pass);
                if (nvs_get_str(legacy, "pass", g_wifi_pass, &sz) != ESP_OK) {
                    sz = sizeof(g_wifi_pass);
                    (void)nvs_get_str(legacy, NVS_KEY_WIFI_PASS, g_wifi_pass, &sz);
                }
            }
            if (g_backend_ip[0] == '\0') {
                sz = sizeof(g_backend_ip);
                (void)nvs_get_str(legacy, NVS_KEY_BACKEND_IP, g_backend_ip, &sz);
            }
            nvs_close(legacy);
        }
    }
    nvs_close(handle);

    str_trim_copy(g_wifi_ssid, sizeof(g_wifi_ssid), g_wifi_ssid);
    str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), g_wifi_pass);
    str_trim_copy(g_backend_ip, sizeof(g_backend_ip), g_backend_ip);
    str_trim_copy(g_backend_port, sizeof(g_backend_port), g_backend_port);

    if (g_wifi_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Loaded NVS config: SSID '%s', backend %s:%s",
                 g_wifi_ssid, g_backend_ip, g_backend_port[0] ? g_backend_port : "8080");
        return true;
    }
    return false;
}

bool wifi_nvs_save_full_config(const char *ssid, const char *pass,
                               const char *backend_ip, const char *backend_port) {
    nvs_handle_t handle;
    if (!ssid || ssid[0] == '\0') return false;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;

    // Save SSID to wifi_ssid and legacy ssid
    (void)nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid);
    (void)nvs_set_str(handle, "ssid", ssid);

    // JANGAN hapus/timpa password dengan string kosong jika parameter pass bernilai NULL/empty.
    // Sebaliknya, pertahankan password yang ada (termasuk migrasi dari namespace lama).
    if (pass && pass[0] != '\0') {
        (void)nvs_set_str(handle, NVS_KEY_WIFI_PASS, pass);
        (void)nvs_set_str(handle, "pass", pass);
    } else {
        char existing_pass[64] = {0};
        size_t psz = sizeof(existing_pass);
        if (nvs_get_str(handle, NVS_KEY_WIFI_PASS, existing_pass, &psz) != ESP_OK) {
            psz = sizeof(existing_pass);
            (void)nvs_get_str(handle, "pass", existing_pass, &psz);
        }
        if (existing_pass[0] == '\0') {
            nvs_handle_t legacy;
            if (nvs_open("wifi_config", NVS_READONLY, &legacy) == ESP_OK) {
                psz = sizeof(existing_pass);
                if (nvs_get_str(legacy, "pass", existing_pass, &psz) != ESP_OK) {
                    psz = sizeof(existing_pass);
                    (void)nvs_get_str(legacy, NVS_KEY_WIFI_PASS, existing_pass, &psz);
                }
                nvs_close(legacy);
            }
        }
        if (existing_pass[0] != '\0') {
            (void)nvs_set_str(handle, NVS_KEY_WIFI_PASS, existing_pass);
            (void)nvs_set_str(handle, "pass", existing_pass);
        }
    }

    if (backend_ip && backend_ip[0] != '\0') {
        (void)nvs_set_str(handle, NVS_KEY_BACKEND_IP, backend_ip);
    }

    uint16_t port_val = 8080;
    if (backend_port && backend_port[0] != '\0') {
        int p = atoi(backend_port);
        if (p > 0 && p <= 65535) port_val = (uint16_t)p;
    }
    (void)nvs_set_u16(handle, NVS_KEY_BACKEND_PORT, port_val);

    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool wifi_nvs_save_credentials(const char *ssid, const char *pass) {
    return wifi_nvs_save_full_config(ssid, pass, NULL, NULL);
}

bool wifi_apply_runtime_credentials(const char *ssid, const char *pass,
                                    const char *backend_ip, const char *backend_port) {
    if (!ssid || ssid[0] == '\0') return false;

    str_trim_copy(g_wifi_ssid, sizeof(g_wifi_ssid), ssid);
    if (pass && pass[0] != '\0') str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), pass);
    if (backend_ip && backend_ip[0] != '\0') str_trim_copy(g_backend_ip, sizeof(g_backend_ip), backend_ip);
    if (backend_port && backend_port[0] != '\0') str_trim_copy(g_backend_port, sizeof(g_backend_port), backend_port);
    s_discovered_ip[0] = '\0';

    wifi_nvs_save_full_config(g_wifi_ssid, (pass && pass[0] != '\0') ? g_wifi_pass : NULL, g_backend_ip, g_backend_port);

    if (s_stream_client != NULL) {
        esp_http_client_close(s_stream_client);
        esp_http_client_cleanup(s_stream_client);
        s_stream_client = NULL;
        s_stream_url[0] = '\0';
    }

    char target_url[URL_BUFFER_SIZE];
    if (get_backend_url(CAMERA_FRAME_ENDPOINT, target_url, sizeof(target_url)) == ESP_OK) {
        ESP_LOGI(TAG, "Target backend URL: %s", target_url);
    }

    if (s_wifi_initialized) {
        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, g_wifi_ssid, sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, g_wifi_pass, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = (g_wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable = true;
        if (g_wifi_event_group) {
            xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        (void)esp_wifi_disconnect();
        (void)esp_wifi_set_config(WIFI_IF_STA, &cfg);
        (void)esp_wifi_connect();
    }
    return true;
}

uint16_t get_backend_port(void) {
    if (g_backend_port[0] != '\0') {
        int p = atoi(g_backend_port);
        if (p > 0 && p <= 65535) return (uint16_t)p;
    }
    return 8080;
}

bool get_backend_ip(char *out_ip, size_t max_len) {
    if (!out_ip || max_len == 0) return false;

    // Prioritas 1 (NVS Custom Gateway): Jika g_backend_ip di NVS valid, gunakan ini mutlak!
    if (is_valid_ipv4(g_backend_ip)) {
        strncpy(out_ip, g_backend_ip, max_len - 1);
        out_ip[max_len - 1] = '\0';
        return true;
    }

    // Prioritas 2 (Fallback DHCP Gateway): Hanya jika NVS kosong/invalid, fallback ke ip_info.gw
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.gw.addr != 0) {
        snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.gw));
        return true;
    }

    // Prioritas 3 (Fallback Kconfig/.env): Terakhir gunakan default compile-time
    if (is_valid_ipv4(CONFIG_BACKEND_IP)) {
        strncpy(out_ip, CONFIG_BACKEND_IP, max_len - 1);
        out_ip[max_len - 1] = '\0';
        return true;
    }

    return false;
}

esp_err_t get_backend_url(const char *endpoint, char *out_url, size_t max_len) {
    if (!endpoint || !out_url || max_len == 0) return ESP_ERR_INVALID_ARG;
    char ip[32] = {0};
    if (!get_backend_ip(ip, sizeof(ip))) return ESP_ERR_NOT_FOUND;
    uint16_t port = get_backend_port();
    snprintf(out_url, max_len, "http://%s:%u%s", ip, port, endpoint);
    return ESP_OK;
}

void wifi_notify_backend_failure(void) {
    s_discovered_ip[0] = '\0';
}

bool wifi_camera_stream_is_healthy(void) {
    return is_camera_streaming();
}

static void wifi_reconnect_timer_cb(void *arg) {
    (void)arg;
    if (g_wifi_ssid[0] != '\0') {
        (void)esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (g_wifi_ssid[0] != '\0') {
            ESP_LOGI(TAG, "Connecting to AP '%s'...", g_wifi_ssid);
            lcd_update_http_status("WIFI CONNECTING...", false);
            (void)esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Wi-Fi SSID not configured. Waiting for USB provisioning.");
            lcd_update_http_status("WAIT USB WIFI...", false);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "Associated with AP, requesting DHCP...");
        lcd_update_http_status("DHCP REQUEST...", false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Connection lost (reason %d)", dis ? dis->reason : -1);
        if (g_wifi_event_group) {
            xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(false, NULL);
        lcd_update_http_status("WIFI LOST", true);
        g_wifi_retry_num++;
        uint32_t delay_ms = (g_wifi_retry_num < 5) ? 1000 : 3000;
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[24] = {0};
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Obtained IP: %s", ip_str);
        g_wifi_retry_num = 0;
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        if (g_wifi_event_group) {
            xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(true, ip_str);
        lcd_update_http_status("WIFI READY", false);

        char target_url[URL_BUFFER_SIZE] = {0};
        if (get_backend_url(CAMERA_FRAME_ENDPOINT, target_url, sizeof(target_url)) == ESP_OK) {
            ESP_LOGI(TAG, "Target backend URL: %s", target_url);
        }
    }
}

void wifi_init_sta(void) {
    if (s_wifi_initialized) return;

    g_wifi_event_group = xEventGroupCreate();

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_reconnect_timer_cb,
        .name = "wifi_reconnect_timer",
    };
    esp_timer_create(&timer_args, &s_reconnect_timer);

    if (!wifi_nvs_load_credentials()) {
        str_trim_copy(g_wifi_ssid, sizeof(g_wifi_ssid), CONFIG_ESP_WIFI_SSID);
        str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), CONFIG_ESP_WIFI_PASSWORD);
        str_trim_copy(g_backend_ip, sizeof(g_backend_ip), CONFIG_BACKEND_IP);
        str_trim_copy(g_backend_port, sizeof(g_backend_port), CONFIG_BACKEND_PORT);
    }

    esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(netif_err);
    }
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, g_wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, g_wifi_pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.threshold.authmode = (g_wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (g_wifi_ssid[0] != '\0') {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    s_wifi_initialized = true;
}

esp_err_t send_camera_frame_to_backend(const char *backend_ip, uint16_t port, const char *device_id) {
    static camera_frame_buffer_t tx_frame = {0};
    char url[URL_BUFFER_SIZE];
    char frame_id_str[16], temp_str[16], load_str[16];

    if (!backend_ip || !device_id || !is_camera_streaming()) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(url, sizeof(url), "http://%s:%u%s", backend_ip, port, CAMERA_FRAME_ENDPOINT);

    if (tx_frame.data == NULL) {
        tx_frame.capacity = INITIAL_BUF_CAPACITY;
        tx_frame.data = (uint8_t *)heap_caps_malloc(tx_frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tx_frame.data == NULL) {
            tx_frame.data = (uint8_t *)malloc(tx_frame.capacity);
        }
        if (tx_frame.data == NULL) return ESP_ERR_NO_MEM;
    }

    if (camera_copy_latest_frame(&tx_frame) != ESP_OK || tx_frame.header.buffer_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_stream_client == NULL || strcmp(s_stream_url, url) != 0) {
        if (s_stream_client != NULL) {
            esp_http_client_close(s_stream_client);
            esp_http_client_cleanup(s_stream_client);
        }
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .buffer_size_tx = STREAM_TX_BUFFER_SIZE,
            .keep_alive_enable = true,
        };
        s_stream_client = esp_http_client_init(&config);
        if (!s_stream_client) return ESP_FAIL;
        strncpy(s_stream_url, url, sizeof(s_stream_url) - 1);
        ESP_LOGI(TAG, "Target backend URL: %s", url);
    }

    static uint32_t s_last_telemetry_check = 0;
    static float s_cached_temp = 50.0f;
    static uint8_t s_cached_load = 5;
    uint32_t now_ticks = (uint32_t)xTaskGetTickCount();
    if (now_ticks - s_last_telemetry_check > pdMS_TO_TICKS(1000) || s_last_telemetry_check == 0) {
        s_cached_temp = get_esp32_cpu_temperature();
        s_cached_load = get_cpu_usage_percentage();
        s_last_telemetry_check = now_ticks;
    }

    snprintf(frame_id_str, sizeof(frame_id_str), "%lu", (unsigned long)tx_frame.header.frame_id);
    snprintf(temp_str, sizeof(temp_str), "%.2f", s_cached_temp);
    snprintf(load_str, sizeof(load_str), "%u", (unsigned)s_cached_load);

    esp_http_client_set_header(s_stream_client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(s_stream_client, "Connection", "keep-alive");
    esp_http_client_set_header(s_stream_client, "X-Device-ID", device_id);
    esp_http_client_set_header(s_stream_client, "X-Device-Token", CONFIG_DEVICE_AUTH_TOKEN);
    esp_http_client_set_header(s_stream_client, "X-Frame-ID", frame_id_str);
    esp_http_client_set_header(s_stream_client, "X-ESP-Temp", temp_str);
    esp_http_client_set_header(s_stream_client, "X-ESP-CPU-Load", load_str);
    esp_http_client_set_header(s_stream_client, "X-Camera-Model", camera_get_sensor_name());
    esp_http_client_set_post_field(s_stream_client, (const char *)tx_frame.data, (int)tx_frame.header.buffer_len);

    static int s_err_count = 0;
    esp_err_t err = esp_http_client_perform(s_stream_client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(s_stream_client);
        if (status >= 200 && status < 300) {
            s_err_count = 0;
            return ESP_OK;
        }
    }
    // Jika transmisi HTTP gagal atau timeout, tutup soket TCP agar tidak mengirim payload ke soket yang korup/setengah-jalan
    esp_http_client_close(s_stream_client);
    s_err_count++;
    if (s_err_count >= 5) {
        wifi_notify_backend_failure();
        s_err_count = 0;
    }
    return ESP_FAIL;
}

esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port, telemetry_data_t *data) {
    if (!backend_ip) return ESP_ERR_INVALID_ARG;
    char url[URL_BUFFER_SIZE];
    snprintf(url, sizeof(url), "http://%s:%u%s", backend_ip, port, BACKEND_ENDPOINT);

    telemetry_data_t meta;
    init_device_metadata(&meta);
    const char *dev_id = (data && data->device_id[0]) ? data->device_id : meta.device_id;
    float temp = get_esp32_cpu_temperature();
    uint8_t load = get_cpu_usage_percentage();

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "device_id", dev_id);
    cJSON_AddStringToObject(root, "firmware_version", meta.firmware_version);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "temperature", (double)temp);
    cJSON_AddNumberToObject(root, "cpu_load", (double)load);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddStringToObject(root, "camera_model", camera_get_sensor_name());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        cJSON_free(json);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-ID", dev_id);
    esp_http_client_set_header(client, "X-Device-Token", CONFIG_DEVICE_AUTH_TOKEN);
    esp_http_client_set_post_field(client, json, (int)strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    cJSON_free(json);
    return err;
}

void http_stream_sender_task(void *pvParameters) {
    (void)pvParameters;
    s_stream_task_handle = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "Stream sender task started (target ~20 FPS, %dms adaptive pacing)", STREAM_TARGET_INTERVAL_MS);

    char backend_ip[32];
    telemetry_data_t meta;
    init_device_metadata(&meta);
    const TickType_t target_interval = pdMS_TO_TICKS(STREAM_TARGET_INTERVAL_MS);

    for (;;) {
        TickType_t start_time = xTaskGetTickCount();

        if (!(g_wifi_event_group && (xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT))) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        esp_err_t res = send_camera_frame_to_backend(backend_ip, get_backend_port(), meta.device_id);
        if (res == ESP_ERR_NOT_FOUND) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        } else if (res != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        TickType_t elapsed = xTaskGetTickCount() - start_time;
        if (elapsed < target_interval) {
            vTaskDelay(target_interval - elapsed);
        } else {
            taskYIELD();
        }
    }
}

void WiFi_Server_Task(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        // While camera is streaming, telemetry is embedded in every frame header (15 FPS).
        // Only emit standalone telemetry if camera stream is idle/standby.
        if (is_camera_streaming()) continue;
        if (!(g_wifi_event_group && (xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT))) continue;

        char backend_ip[32];
        if (get_backend_ip(backend_ip, sizeof(backend_ip))) {
            (void)send_telemetry_to_backend(backend_ip, get_backend_port(), NULL);
        }
    }
}

static void usb_send_ack(const char *status) {
    telemetry_data_t meta;
    init_device_metadata(&meta);
    cJSON *ack = cJSON_CreateObject();
    if (!ack) return;

    bool connected = (g_wifi_event_group && (xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT));
    cJSON_AddStringToObject(ack, "status", status);
    cJSON_AddStringToObject(ack, "device_id", meta.device_id);
    cJSON_AddStringToObject(ack, "firmware_version", meta.firmware_version);
    cJSON_AddBoolToObject(ack, "wifi_connected", connected);
    cJSON_AddStringToObject(ack, "ssid", g_wifi_ssid);
    cJSON_AddStringToObject(ack, "backend_ip", g_backend_ip);
    cJSON_AddNumberToObject(ack, "backend_port", (double)get_backend_port());
    cJSON_AddNumberToObject(ack, "temperature", (double)get_esp32_cpu_temperature());
    cJSON_AddNumberToObject(ack, "cpu_temperature", (double)get_esp32_cpu_temperature());
    cJSON_AddNumberToObject(ack, "cpu_load", (double)get_cpu_usage_percentage());

    char *str = cJSON_PrintUnformatted(ack);
    if (str) {
        char ack_line[512];
        int n = snprintf(ack_line, sizeof(ack_line), "\nBAWAL_ACK:%s\n", str);
        if (n > 0) {
            // printf goes to the UART0 console (/dev/ttyACM0); the native
            // USB-Serial-JTAG port gets the same ACK when it is connected.
            printf("%s", ack_line);
            fflush(stdout);
            (void)usb_serial_jtag_write_bytes(ack_line, (size_t)n, pdMS_TO_TICKS(50));
        }
        cJSON_free(str);
    }
    cJSON_Delete(ack);
}

static void handle_provisioning_line(char *line) {
    char *json_start = strchr(line, '{');
    if (!json_start) return;

    cJSON *cmd_json = cJSON_Parse(json_start);
    if (!cmd_json) return;

    cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "cmd");
    if (cJSON_IsString(cmd_item) && cmd_item->valuestring) {
        if (strcmp(cmd_item->valuestring, "set_wifi") == 0 ||
            strcmp(cmd_item->valuestring, "provision_wifi") == 0) {
            cJSON *s = cJSON_GetObjectItemCaseSensitive(cmd_json, "ssid");
            cJSON *p = cJSON_GetObjectItemCaseSensitive(cmd_json, "pass");
            if (!p) p = cJSON_GetObjectItemCaseSensitive(cmd_json, "password");
            cJSON *ip = cJSON_GetObjectItemCaseSensitive(cmd_json, "backend_ip");
            if (!ip) ip = cJSON_GetObjectItemCaseSensitive(cmd_json, "gateway_host_ip");
            cJSON *port = cJSON_GetObjectItemCaseSensitive(cmd_json, "backend_port");

            const char *ssid_val = (cJSON_IsString(s) && s->valuestring) ? s->valuestring : "";
            const char *pass_val = (cJSON_IsString(p) && p->valuestring) ? p->valuestring : "";
            const char *ip_val = (cJSON_IsString(ip) && ip->valuestring) ? ip->valuestring : "";

            char port_buf[16] = "8080";
            if (cJSON_IsNumber(port)) {
                snprintf(port_buf, sizeof(port_buf), "%d", port->valueint);
            } else if (cJSON_IsString(port) && port->valuestring && port->valuestring[0] != '\0') {
                strncpy(port_buf, port->valuestring, sizeof(port_buf) - 1);
            }

            bool ok = wifi_apply_runtime_credentials(ssid_val, pass_val, ip_val, port_buf);
            if (ok && g_wifi_event_group) {
                (void)xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
            }
            usb_send_ack(ok ? "wifi_provisioned" : "error");
        } else if (strcmp(cmd_item->valuestring, "status") == 0 ||
                   strcmp(cmd_item->valuestring, "get_status") == 0) {
            usb_send_ack("ok");
        }
    }
    cJSON_Delete(cmd_json);
}

static void provisioning_feed_bytes(const uint8_t *data, size_t len,
                                    char *line, size_t line_sz, size_t *idx) {
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch == '\r' || ch == '\n') {
            if (*idx == 0) continue;
            line[*idx] = '\0';
            *idx = 0;
            handle_provisioning_line(line);
        } else if (*idx < line_sz - 1) {
            if ((unsigned char)ch >= 0x20 || ch == '\t') {
                line[(*idx)++] = ch;
            }
        } else {
            *idx = 0;
        }
    }
}

void usb_serial_provisioning_task(void *pvParameters) {
    (void)pvParameters;
    char line[256];
    size_t idx = 0;
    uint8_t buf[128];

    for (;;) {
        // Native USB-Serial-JTAG (if connected).
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
        if (n > 0) {
            provisioning_feed_bytes(buf, (size_t)n, line, sizeof(line), &idx);
        }

        // UART0 console (/dev/ttyACM0 via CH343 bridge) through the console
        // VFS. select() avoids blocking the task when no JSON is pending.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0) {
            ssize_t m = read(STDIN_FILENO, buf, sizeof(buf));
            if (m > 0) {
                provisioning_feed_bytes(buf, (size_t)m, line, sizeof(line), &idx);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

