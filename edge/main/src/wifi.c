#include "wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
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
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
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

#define MJPEG_CLIENT_LIMIT 2
#define MJPEG_FRAME_CAPACITY INITIAL_BUF_CAPACITY
#define TELEMETRY_ENDPOINT "/api/v1/telemetry"
#define TELEMETRY_URL_BUFFER_SIZE 128

EventGroupHandle_t g_wifi_event_group = NULL;
int g_wifi_retry_num = 0;

/* Buffer kredensial runtime dinamis */
char g_wifi_ssid[32] = {0};
char g_wifi_pass[64] = {0};
char g_backend_ip[32] = {0};
char g_backend_port[8] = "8080";

static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_wifi_initialized = false;
static httpd_handle_t s_http_server = NULL;
static atomic_int s_mjpeg_clients = 0;

void wifi_notify_stream_frame_ready(void) {
    /* Feed pull-based HTTP MJPEG stream secara asinkron */
}

/* Sanitasi input: membuang spasi, tab, newline, dan tanda kutip ganda akibat JSON formatting */
static void str_sanitize_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n' || *src == '\"') {
        src++;
    }
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                       src[len - 1] == '\r' || src[len - 1] == '\n' || src[len - 1] == '\"')) {
        len--;
    }
    if (len >= dst_sz) {
        len = dst_sz - 1;
    }
    memmove(dst, src, len);
    dst[len] = '\0';
}

static bool is_valid_ipv4(const char *ip) {
    if (!ip || strlen(ip) < 7 || strlen(ip) > 15) return false;
    if (strcmp(ip, "0.0.0.0") == 0 || strcmp(ip, "255.255.255.255") == 0) return false;
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1 && addr.s_addr != 0;
}

static bool get_sta_ip_info(esp_netif_ip_info_t *info) {
    if (!info) return false;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_default_netif();
    }
    return netif && esp_netif_get_ip_info(netif, info) == ESP_OK &&
           info->ip.addr != 0 && info->netmask.addr != 0;
}

static bool is_same_subnet_ip(const char *ip) {
    struct in_addr addr;
    esp_netif_ip_info_t info;
    if (!is_valid_ipv4(ip) || inet_pton(AF_INET, ip, &addr) != 1) return false;
    if (!get_sta_ip_info(&info)) return false;

    uint32_t ip_host = ntohl(addr.s_addr);
    uint32_t esp_host = ntohl(info.ip.addr);
    uint32_t mask_host = ntohl(info.netmask.addr);
    return (ip_host & mask_host) == (esp_host & mask_host);
}

bool wifi_nvs_load_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            return false;
        }
    }

    char tmp_ssid[32] = {0};
    char tmp_pass[64] = {0};
    char tmp_ip[32] = {0};
    char tmp_port[8] = {0};

    size_t sz = sizeof(tmp_ssid);
    if (nvs_get_str(handle, "ssid", tmp_ssid, &sz) != ESP_OK) {
        sz = sizeof(tmp_ssid);
        (void)nvs_get_str(handle, NVS_KEY_WIFI_SSID, tmp_ssid, &sz);
    }

    sz = sizeof(tmp_pass);
    if (nvs_get_str(handle, "pass", tmp_pass, &sz) != ESP_OK) {
        sz = sizeof(tmp_pass);
        (void)nvs_get_str(handle, NVS_KEY_WIFI_PASS, tmp_pass, &sz);
    }

    sz = sizeof(tmp_ip);
    if (nvs_get_str(handle, "backend_ip", tmp_ip, &sz) != ESP_OK) {
        sz = sizeof(tmp_ip);
        (void)nvs_get_str(handle, NVS_KEY_BACKEND_IP, tmp_ip, &sz);
    }

    uint16_t port_u16 = 8080;
    if (nvs_get_u16(handle, NVS_KEY_BACKEND_PORT, &port_u16) == ESP_OK && port_u16 > 0) {
        snprintf(tmp_port, sizeof(tmp_port), "%u", port_u16);
    } else {
        sz = sizeof(tmp_port);
        (void)nvs_get_str(handle, "backend_port", tmp_port, &sz);
    }

    nvs_close(handle);

    str_sanitize_copy(g_wifi_ssid, sizeof(g_wifi_ssid), tmp_ssid);
    str_sanitize_copy(g_wifi_pass, sizeof(g_wifi_pass), tmp_pass);
    str_sanitize_copy(g_backend_ip, sizeof(g_backend_ip), tmp_ip);
    str_sanitize_copy(g_backend_port, sizeof(g_backend_port), tmp_port);

    if (g_wifi_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Kredensial terbaca dari NVS: SSID '%s', Target %s:%s",
                 g_wifi_ssid, g_backend_ip, g_backend_port[0] ? g_backend_port : "8080");
        return true;
    }
    return false;
}

bool wifi_nvs_save_full_config(const char *ssid, const char *pass,
                               const char *backend_ip, const char *backend_port) {
    nvs_handle_t handle;
    if (!ssid || ssid[0] == '\0') return false;

    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) return false;
    }

    (void)nvs_set_str(handle, "ssid", ssid);
    (void)nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid);

    if (pass) {
        (void)nvs_set_str(handle, "pass", pass);
        (void)nvs_set_str(handle, NVS_KEY_WIFI_PASS, pass);
    }

    if (backend_ip && backend_ip[0] != '\0' && is_valid_ipv4(backend_ip)) {
        (void)nvs_set_str(handle, "backend_ip", backend_ip);
        (void)nvs_set_str(handle, NVS_KEY_BACKEND_IP, backend_ip);
    }

    if (backend_port && backend_port[0] != '\0') {
        (void)nvs_set_str(handle, "backend_port", backend_port);
        uint16_t p = (uint16_t)atoi(backend_port);
        if (p > 0) {
            (void)nvs_set_u16(handle, NVS_KEY_BACKEND_PORT, p);
        }
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool wifi_apply_runtime_credentials(const char *ssid, const char *pass,
                                    const char *backend_ip, const char *backend_port) {
    if (!ssid || ssid[0] == '\0') return false;

    str_sanitize_copy(g_wifi_ssid, sizeof(g_wifi_ssid), ssid);
    if (pass) {
        str_sanitize_copy(g_wifi_pass, sizeof(g_wifi_pass), pass);
    }
    if (backend_ip && backend_ip[0] != '\0' && is_valid_ipv4(backend_ip)) {
        str_sanitize_copy(g_backend_ip, sizeof(g_backend_ip), backend_ip);
    }
    if (backend_port && backend_port[0] != '\0') {
        str_sanitize_copy(g_backend_port, sizeof(g_backend_port), backend_port);
    }

    wifi_nvs_save_full_config(g_wifi_ssid, g_wifi_pass, g_backend_ip, g_backend_port);

    if (s_wifi_initialized) {
        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, g_wifi_ssid, sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, g_wifi_pass, sizeof(cfg.sta.password) - 1);
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

        /* Support PMF-capable hotspots while leaving PMF optional for WPA2. */
        cfg.sta.threshold.authmode = (strlen(g_wifi_pass) > 0) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable = false;
        cfg.sta.pmf_cfg.required = false;
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        cfg.sta.sae_pk_mode = WPA3_SAE_PK_MODE_DISABLED;

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

    esp_netif_ip_info_t ip_info = {0};
    if (get_sta_ip_info(&ip_info)) {
        /* Prioritas 1: Jika konfigurasi IP backend cocok dengan subnet lokal */
        if (is_valid_ipv4(g_backend_ip) && is_same_subnet_ip(g_backend_ip)) {
            snprintf(out_ip, max_len, "%s", g_backend_ip);
            return true;
        }
        /* Prioritas 2: Fallback otomatis ke IP Gateway router/hotspot */
        if (ip_info.gw.addr != 0) {
            snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.gw));
            return true;
        }
    }

    /* Fallback terakhir sebelum DHCP siap */
    if (is_valid_ipv4(g_backend_ip)) {
        snprintf(out_ip, max_len, "%s", g_backend_ip);
        return true;
    }
    return false;
}

esp_err_t get_backend_url(const char *endpoint, char *out_url, size_t max_len) {
    if (!endpoint || !out_url || max_len == 0) return ESP_ERR_INVALID_ARG;
    char ip[INET_ADDRSTRLEN] = {0};
    if (!get_backend_ip(ip, sizeof(ip))) return ESP_ERR_NOT_FOUND;
    int n = snprintf(out_url, max_len, "http://%s:%u%s", ip, get_backend_port(), endpoint);
    return (n > 0 && (size_t)n < max_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port,
                                           telemetry_data_t *data) {
    if (!backend_ip || !is_valid_ipv4(backend_ip)) return ESP_ERR_INVALID_ARG;
    char url[TELEMETRY_URL_BUFFER_SIZE];
    int url_len = snprintf(url, sizeof(url), "http://%s:%u%s", backend_ip, port, TELEMETRY_ENDPOINT);
    if (url_len <= 0 || (size_t)url_len >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

    telemetry_data_t meta;
    init_device_metadata(&meta);
    const char *dev_id = (data && data->device_id[0]) ? data->device_id : meta.device_id;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "device_id", dev_id);
    cJSON_AddStringToObject(root, "firmware_version", meta.firmware_version);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "temperature", (double)get_esp32_cpu_temperature());
    cJSON_AddNumberToObject(root, "cpu_load", (double)get_cpu_usage_percentage());
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_heap_internal", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_heap_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(root, "camera_model", camera_get_sensor_name());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 1500,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        cJSON_free(json);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "X-Device-ID", dev_id);
    esp_http_client_set_header(client, "X-Device-Token", CONFIG_DEVICE_AUTH_TOKEN);
    esp_http_client_set_post_field(client, json, (int)strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    cJSON_free(json);
    return err;
}

void WiFi_Server_Task(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!g_wifi_event_group || !(xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            continue;
        }

        char backend_ip[INET_ADDRSTRLEN] = {0};
        if (get_backend_ip(backend_ip, sizeof(backend_ip))) {
            (void)send_telemetry_to_backend(backend_ip, get_backend_port(), NULL);
        }
    }
}

static void wifi_reconnect_timer_cb(void *arg) {
    (void)arg;
    if (g_wifi_ssid[0] != '\0') {
        (void)esp_wifi_connect();
    }
}

static const char *wifi_authmode_name(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
        case WIFI_AUTH_WPA3_PSK: return "WPA3-SAE";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-SAE";
        case WIFI_AUTH_UNKNOWN: return "UNKNOWN";
        default: return "OTHER";
    }
}

static void wifi_scan_then_connect_task(void *arg) {
    (void)arg;
    wifi_scan_config_t scan = {
        .ssid = (uint8_t *)g_wifi_ssid,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostic scan failed: %s", esp_err_to_name(err));
    } else {
        uint16_t ap_count = 0;
        err = esp_wifi_scan_get_ap_num(&ap_count);
        if (err != ESP_OK || ap_count == 0) {
            ESP_LOGW(TAG, "Diagnostic scan found no AP for SSID '%s'", g_wifi_ssid);
        } else {
            wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
            if (!records) {
                ESP_LOGW(TAG, "Diagnostic scan: %u AP records; allocation failed", ap_count);
            } else {
                uint16_t record_count = ap_count;
                err = esp_wifi_scan_get_ap_records(&record_count, records);
                if (err == ESP_OK) {
                    for (uint16_t i = 0; i < record_count; ++i) {
                        ESP_LOGI(TAG,
                                 "AP candidate: auth=%s(%d), channel=%u, rssi=%d, pairwise=%d, group=%d",
                                 wifi_authmode_name(records[i].authmode), records[i].authmode,
                                 records[i].primary, records[i].rssi,
                                 records[i].pairwise_cipher, records[i].group_cipher);
                    }
                } else {
                    ESP_LOGW(TAG, "Could not read diagnostic scan records: %s",
                             esp_err_to_name(err));
                }
                free(records);
            }
        }
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial Wi-Fi connect failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (g_wifi_ssid[0] != '\0') {
            ESP_LOGI(TAG, "Menghubungkan ke AP '%s'...", g_wifi_ssid);
            lcd_update_http_status("WIFI CONNECTING...", false);
            if (xTaskCreate(wifi_scan_then_connect_task, "wifi_scan_diag", 4096,
                            NULL, 3, NULL) != pdPASS) {
                ESP_LOGW(TAG, "Diagnostic scan task unavailable; connecting directly");
                (void)esp_wifi_connect();
            }
        } else {
            ESP_LOGW(TAG, "SSID belum diatur. Menunggu input serial provisioning.");
            lcd_update_http_status("WAIT USB PROV...", false);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "Asosiasi AP berhasil, meminta alamat IP via DHCP...");
        lcd_update_http_status("DHCP LEASE...", false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        int reason = dis ? dis->reason : -1;
        ESP_LOGW(TAG, "Koneksi terputus (Reason code: %d)", reason);

        if (g_wifi_event_group) {
            xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(false, NULL);
        lcd_update_http_status("WIFI RETRY", true);

        g_wifi_retry_num++;
        uint32_t delay_ms = (g_wifi_retry_num < 4) ? 800 : 2500;
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[24] = {0};
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Link Wi-Fi Aktif! IP: %s | GW: " IPSTR, ip_str, IP2STR(&event->ip_info.gw));
        g_wifi_retry_num = 0;
        (void)esp_wifi_set_ps(WIFI_PS_NONE);

        if (g_wifi_event_group) {
            xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(true, ip_str);
        lcd_update_http_status("ONLINE 200 OK", false);
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

    /* Hierarki Kredensial: NVS -> Kconfig Project Fallback */
    if (!wifi_nvs_load_credentials()) {
        str_sanitize_copy(g_wifi_ssid, sizeof(g_wifi_ssid), CONFIG_ESP_WIFI_SSID);
        str_sanitize_copy(g_wifi_pass, sizeof(g_wifi_pass), CONFIG_ESP_WIFI_PASSWORD);
        str_sanitize_copy(g_backend_ip, sizeof(g_backend_ip), CONFIG_BACKEND_IP);
        str_sanitize_copy(g_backend_port, sizeof(g_backend_port), CONFIG_BACKEND_PORT);
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

    /* Gunakan authmode threshold adaptif agar kompatibel dengan WPA2 dan Open */
    wifi_cfg.sta.threshold.authmode = (strlen(g_wifi_pass) > 0) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    /* Support PMF-capable hotspots while leaving PMF optional for WPA2. */
    wifi_cfg.sta.pmf_cfg.capable = false;
    wifi_cfg.sta.pmf_cfg.required = false;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_cfg.sta.sae_pk_mode = WPA3_SAE_PK_MODE_DISABLED;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (g_wifi_ssid[0] != '\0') {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    s_wifi_initialized = true;
}

static esp_err_t snapshot_handler(httpd_req_t *req) {
    camera_frame_buffer_t frame = {0};
    frame.capacity = MJPEG_FRAME_CAPACITY;
    frame.data = heap_caps_malloc(frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame.data == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = camera_copy_current_frame(&frame);
    if (err == ESP_OK && frame.header.buffer_len > 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        err = httpd_resp_send(req, (const char *)frame.data, frame.header.buffer_len);
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Frame belum tersedia");
    }
    free(frame.data);
    return err;
}

static void mjpeg_stream_task(void *arg) {
    httpd_req_t *req = (httpd_req_t *)arg;
    camera_frame_buffer_t frame = {0};
    frame.capacity = MJPEG_FRAME_CAPACITY;
    frame.data = heap_caps_malloc(frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (frame.data != NULL) {
        httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        uint32_t last_frame_id = UINT32_MAX;
        bool first = true;

        for (;;) {
            esp_err_t copied = camera_copy_current_frame(&frame);
            if (copied != ESP_OK || frame.header.buffer_len == 0 ||
                frame.header.frame_id == last_frame_id) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            char part_header[96];
            int header_len = snprintf(part_header, sizeof(part_header),
                "%s--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                first ? "" : "\r\n", (unsigned)frame.header.buffer_len);
            if (header_len <= 0 ||
                httpd_resp_send_chunk(req, part_header, header_len) != ESP_OK ||
                httpd_resp_send_chunk(req, (const char *)frame.data,
                                      (ssize_t)frame.header.buffer_len) != ESP_OK) {
                break;
            }
            last_frame_id = frame.header.frame_id;
            first = false;
        }
        free(frame.data);
    }

    (void)httpd_resp_send_chunk(req, NULL, 0);
    (void)httpd_req_async_handler_complete(req);
    atomic_fetch_sub(&s_mjpeg_clients, 1);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req) {
    if (atomic_load(&s_mjpeg_clients) >= MJPEG_CLIENT_LIMIT) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Koneksi stream penuh");
    }

    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) return err;

    atomic_fetch_add(&s_mjpeg_clients, 1);
    if (xTaskCreate(mjpeg_stream_task, "mjpeg_stream", 4096, async_req, 5, NULL) != pdPASS) {
        atomic_fetch_sub(&s_mjpeg_clients, 1);
        (void)httpd_req_async_handler_complete(async_req);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_http_stream_server_start(void) {
    if (s_http_server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) return err;

    const httpd_uri_t stream_uri = {
        .uri = "/stream", .method = HTTP_GET,
        .handler = stream_handler, .user_ctx = NULL
    };
    const httpd_uri_t snapshot_uri = {
        .uri = "/snapshot", .method = HTTP_GET,
        .handler = snapshot_handler, .user_ctx = NULL
    };
    if ((err = httpd_register_uri_handler(s_http_server, &stream_uri)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_http_server, &snapshot_uri)) != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MJPEG Server siap melayani port 80: GET /stream & GET /snapshot");
    return ESP_OK;
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
    cJSON_AddNumberToObject(ack, "cpu_load", (double)get_cpu_usage_percentage());

    char *str = cJSON_PrintUnformatted(ack);
    if (str) {
        char ack_line[512];
        int n = snprintf(ack_line, sizeof(ack_line), "\nBAWAL_ACK:%s\n", str);
        if (n > 0) {
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
                (void)xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
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
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
        if (n > 0) {
            provisioning_feed_bytes(buf, (size_t)n, line, sizeof(line), &idx);
        }

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
