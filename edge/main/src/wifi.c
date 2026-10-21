#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "data.h"
#include "wifi.h"
#include "lcd_task.h"
#include "camera_task.h"
#include "system_monitor.h"

#define HTTP_TIMEOUT_MS 2500
#define STREAM_TX_BUFFER_SIZE 4096
#define DISCOVERY_RETRY_MS 5000
#define DISCOVERY_TOTAL_TIMEOUT_MS 700
#define RECONNECT_MIN_MS 500
#define RECONNECT_MAX_MS 8000
#define STREAM_FAILURE_RESET_THRESHOLD 3

EventGroupHandle_t g_wifi_event_group = NULL;
int g_wifi_retry_num = 0;

char g_wifi_ssid[32] = {0};
char g_wifi_pass[64] = {0};
char g_backend_ip[32] = {0};
char g_backend_port[8] = {0};

static const char *TAG = "TELEMETRY_DISPATCH";
static const char *TAG_WIFI = "WIFI_STA";

static SemaphoreHandle_t s_state_lock = NULL;
static char s_discovered_backend_ip[32] = {0};
static volatile bool s_backend_discovery_pending = true;
static volatile bool s_discovery_in_progress = false;
static TickType_t s_last_discovery_tick = 0;

static esp_http_client_handle_t s_telemetry_client = NULL;
static char s_telemetry_url[URL_BUFFER_SIZE] = {0};
static volatile bool s_telemetry_reset_pending = false;

static esp_http_client_handle_t s_stream_client = NULL;
static char s_stream_url[URL_BUFFER_SIZE] = {0};
static volatile bool s_stream_reset_pending = false;
static volatile TickType_t s_last_stream_ok_tick = 0;
static volatile bool s_stream_has_succeeded = false;
static uint8_t s_stream_failures = 0;

static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_wifi_initialized = false;

static void copy_string(char *dst, size_t dst_size, const char *src) {
    size_t len;
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    if (len > 0) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static bool is_valid_ipv4_str(const char *ip) {
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int d = 0;
    char tail = '\0';

    if (ip == NULL || ip[0] < '0' || ip[0] > '9') {
        return false;
    }
    if (sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

static bool is_safe_ssid(const char *ssid) {
    size_t len;
    size_t i;

    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    len = strlen(ssid);
    if (len >= sizeof(g_wifi_ssid)) {
        return false;
    }
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool is_safe_password(const char *pass) {
    size_t len;
    size_t i;

    if (pass == NULL) {
        return false;
    }
    len = strlen(pass);
    if (len > sizeof(g_wifi_pass) - 1) {
        return false;
    }
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)pass[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool parse_backend_port(const char *port_str, uint16_t *out_port) {
    char *end = NULL;
    long value;

    if (port_str == NULL || port_str[0] < '0' || port_str[0] > '9' || out_port == NULL) {
        return false;
    }
    value = strtol(port_str, &end, 10);
    if (end == port_str || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }
    *out_port = (uint16_t)value;
    return true;
}

static void state_lock(void) {
    if (s_state_lock != NULL) {
        (void)xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static void state_unlock(void) {
    if (s_state_lock != NULL) {
        (void)xSemaphoreGive(s_state_lock);
    }
}

static void wifi_reset_telemetry_client(void) {
    if (s_telemetry_client != NULL) {
        esp_http_client_close(s_telemetry_client);
        esp_http_client_cleanup(s_telemetry_client);
        s_telemetry_client = NULL;
    }
    s_telemetry_url[0] = '\0';
}

static void wifi_reset_stream_client(void) {
    if (s_stream_client != NULL) {
        esp_http_client_close(s_stream_client);
        esp_http_client_cleanup(s_stream_client);
        s_stream_client = NULL;
    }
    s_stream_url[0] = '\0';
}

static void wifi_request_http_reset(void) {
    s_telemetry_reset_pending = true;
    s_stream_reset_pending = true;
    s_stream_has_succeeded = false;
    s_last_stream_ok_tick = 0;
}

static void backend_request_discovery(void) {
    state_lock();
    s_discovered_backend_ip[0] = '\0';
    s_backend_discovery_pending = true;
    s_last_discovery_tick = 0;
    state_unlock();
}

static void backend_mark_reachable(void) {
    state_lock();
    s_backend_discovery_pending = false;
    state_unlock();
}

static bool backend_static_ip_available(void) {
    return g_backend_ip[0] != '\0' || is_valid_ipv4_str(CONFIG_BACKEND_IP);
}

static void backend_network_changed(void) {
    state_lock();
    s_discovered_backend_ip[0] = '\0';
    s_backend_discovery_pending = !backend_static_ip_available();
    s_last_discovery_tick = 0;
    state_unlock();
}

static bool wifi_discover_backend_ip(char *out_ip, size_t max_len) {
    int sock;
    int broadcast_enabled = 1;
    struct timeval recv_timeout = {0};
    struct sockaddr_in dest;
    esp_netif_t *netif;
    esp_netif_ip_info_t ip_info;
    const char *probe;
    size_t probe_len;
    bool sent = false;
    bool found = false;
    TickType_t deadline;
    char response[192];

    if (out_ip == NULL || max_len == 0) {
        return false;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGD(TAG, "Discovery socket creation failed (errno %d)", errno);
        return false;
    }

    (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enabled, sizeof(broadcast_enabled));
    recv_timeout.tv_sec = 0;
    recv_timeout.tv_usec = 200000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(BACKEND_DISCOVERY_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    probe = BACKEND_DISCOVERY_MAGIC;
    probe_len = strlen(probe);

    if (sendto(sock, probe, probe_len, 0, (struct sockaddr *)&dest, sizeof(dest)) >= 0) {
        sent = true;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        uint32_t mask = ntohl(ip_info.netmask.addr);
        if (mask != 0 && mask != 0xFFFFFFFFU) {
            uint32_t broadcast = (ntohl(ip_info.ip.addr) & mask) | ~mask;
            dest.sin_addr.s_addr = htonl(broadcast);
            if (sendto(sock, probe, probe_len, 0, (struct sockaddr *)&dest, sizeof(dest)) >= 0) {
                sent = true;
            }
        }
    }

    if (!sent) {
        ESP_LOGD(TAG, "Discovery probe could not be sent (errno %d)", errno);
        close(sock);
        return false;
    }

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DISCOVERY_TOTAL_TIMEOUT_MS);

    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int received;
        char candidate[32] = {0};
        bool valid_response = false;

        memset(&from, 0, sizeof(from));
        received = recvfrom(sock, response, sizeof(response) - 1, 0,
                            (struct sockaddr *)&from, &from_len);
        if (received <= 0) {
            continue;
        }
        response[received] = '\0';

        cJSON *root = cJSON_Parse(response);
        if (root != NULL) {
            cJSON *service = cJSON_GetObjectItemCaseSensitive(root, "service");
            if (cJSON_IsString(service) && service->valuestring != NULL &&
                strcmp(service->valuestring, "bawal-vision") == 0) {
                cJSON *ip_item = cJSON_GetObjectItemCaseSensitive(root, "backend_ip");
                valid_response = true;
                if (cJSON_IsString(ip_item) && is_valid_ipv4_str(ip_item->valuestring)) {
                    copy_string(candidate, sizeof(candidate), ip_item->valuestring);
                }
            }
            cJSON_Delete(root);
        } else if (strncmp(response, "BAWAL_BACKEND", strlen("BAWAL_BACKEND")) == 0) {
            valid_response = true;
        }

        if (!valid_response) {
            continue;
        }

        if (candidate[0] == '\0') {
            uint32_t addr = ntohl(from.sin_addr.s_addr);
            snprintf(candidate, sizeof(candidate), "%u.%u.%u.%u",
                     (unsigned)((addr >> 24) & 0xFFU),
                     (unsigned)((addr >> 16) & 0xFFU),
                     (unsigned)((addr >> 8) & 0xFFU),
                     (unsigned)(addr & 0xFFU));
        }

        copy_string(out_ip, max_len, candidate);
        found = true;
        break;
    }

    close(sock);

    if (found) {
        ESP_LOGI(TAG, "Discovered backend at %s:%d via UDP broadcast",
                 out_ip, BACKEND_DISCOVERY_PORT);
    } else {
        ESP_LOGD(TAG, "No backend discovery response on UDP port %d", BACKEND_DISCOVERY_PORT);
    }
    return found;
}

static bool format_url(char *out_url, size_t max_len, const char *ip,
                       uint16_t port, const char *path) {
    int written;

    if (out_url == NULL || max_len == 0 || ip == NULL || path == NULL) {
        return false;
    }
    written = snprintf(out_url, max_len, "http://%s:%u%s", ip, (unsigned)port, path);
    return written > 0 && (size_t)written < max_len;
}

static bool wifi_get_local_ip(char *out_ip, size_t max_len) {
    esp_netif_t *netif;
    esp_netif_ip_info_t ip_info;

    if (out_ip == NULL || max_len == 0) {
        return false;
    }
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        netif = esp_netif_get_default_netif();
    }
    if (netif == NULL) {
        return false;
    }
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }
    snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

static bool wifi_is_connected(void) {
    if (g_wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

static uint32_t reconnect_backoff_ms(int attempt) {
    uint32_t backoff = RECONNECT_MIN_MS;

    if (attempt > 1) {
        uint32_t shift = (uint32_t)(attempt - 1);
        if (shift > 4U) {
            shift = 4U;
        }
        backoff <<= shift;
    }
    if (backoff > RECONNECT_MAX_MS) {
        backoff = RECONNECT_MAX_MS;
    }
    return backoff;
}

static void wifi_reconnect_timer_cb(void *arg) {
    char ssid[sizeof(g_wifi_ssid)];

    (void)arg;

    state_lock();
    copy_string(ssid, sizeof(ssid), g_wifi_ssid);
    state_unlock();

    if (ssid[0] == '\0' || wifi_is_connected()) {
        return;
    }
    ESP_LOGI(TAG_WIFI, "Retrying connection to AP '%s'...", ssid);
    (void)esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        char ssid[sizeof(g_wifi_ssid)];

        wifi_request_http_reset();
        backend_network_changed();

        state_lock();
        copy_string(ssid, sizeof(ssid), g_wifi_ssid);
        state_unlock();

        if (ssid[0] != '\0') {
            ESP_LOGI(TAG_WIFI, "Connecting to AP '%s'...", ssid);
            lcd_update_http_status("WIFI CONNECTING...", false);
            (void)esp_wifi_connect();
        } else {
            ESP_LOGW(TAG_WIFI, "Wi-Fi SSID not configured. Waiting for USB provisioning.");
            lcd_update_http_status("WAIT USB WIFI...", false);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_ap_record_t ap_info;

        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG_WIFI, "Associated (channel %d, RSSI %d, authmode %d), requesting DHCP...",
                     ap_info.primary, ap_info.rssi, (int)ap_info.authmode);
        } else {
            ESP_LOGI(TAG_WIFI, "Associated, requesting DHCP...");
        }
        lcd_update_http_status("DHCP REQUEST...", false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        char ssid[sizeof(g_wifi_ssid)];
        int attempt;

        wifi_request_http_reset();
        backend_network_changed();

        ESP_LOGW(TAG_WIFI, "Connection lost (reason %d)", dis != NULL ? dis->reason : -1);
        if (g_wifi_event_group != NULL) {
            (void)xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(false, NULL);
        lcd_update_http_status("WIFI LOST", true);

        state_lock();
        copy_string(ssid, sizeof(ssid), g_wifi_ssid);
        if (ssid[0] != '\0') {
            g_wifi_retry_num++;
        }
        attempt = g_wifi_retry_num;
        state_unlock();

        if (ssid[0] == '\0') {
            return;
        }

        uint32_t backoff_ms = reconnect_backoff_ms(attempt);
        ESP_LOGW(TAG_WIFI, "Reconnecting to AP '%s' in %lu ms (attempt %d)...",
                 ssid, (unsigned long)backoff_ms, attempt);

        if (s_reconnect_timer != NULL) {
            (void)esp_timer_stop(s_reconnect_timer);
            if (esp_timer_start_once(s_reconnect_timer, (uint64_t)backoff_ms * 1000ULL) != ESP_OK) {
                (void)esp_wifi_connect();
            }
        } else {
            (void)esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[24];

        if (event == NULL) {
            return;
        }
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_request_http_reset();
        backend_network_changed();

        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG_WIFI, "Obtained IP: %s", ip_str);

        state_lock();
        g_wifi_retry_num = 0;
        state_unlock();

        if (g_wifi_event_group != NULL) {
            (void)xEventGroupClearBits(g_wifi_event_group, WIFI_FAIL_BIT);
            (void)xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(true, ip_str);
        lcd_update_http_status("WIFI READY", false);
    }
}

bool get_backend_ip(char *out_ip, size_t max_len) {
    bool should_discover = false;
    bool resolved = false;

    if (out_ip == NULL || max_len == 0) {
        return false;
    }

    state_lock();
    if (s_backend_discovery_pending && !s_discovery_in_progress) {
        TickType_t now = xTaskGetTickCount();
        if (s_last_discovery_tick == 0 ||
            (TickType_t)(now - s_last_discovery_tick) >= pdMS_TO_TICKS(DISCOVERY_RETRY_MS)) {
            s_last_discovery_tick = now;
            s_discovery_in_progress = true;
            should_discover = true;
        }
    }
    state_unlock();

    if (should_discover) {
        char discovered[32] = {0};
        bool found = wifi_discover_backend_ip(discovered, sizeof(discovered));

        state_lock();
        if (found) {
            copy_string(s_discovered_backend_ip, sizeof(s_discovered_backend_ip), discovered);
            s_backend_discovery_pending = false;
        }
        s_discovery_in_progress = false;
        state_unlock();
    }

    state_lock();
    if (s_discovered_backend_ip[0] != '\0') {
        copy_string(out_ip, max_len, s_discovered_backend_ip);
        resolved = true;
    } else if (g_backend_ip[0] != '\0') {
        copy_string(out_ip, max_len, g_backend_ip);
        resolved = true;
    } else if (is_valid_ipv4_str(CONFIG_BACKEND_IP)) {
        copy_string(out_ip, max_len, CONFIG_BACKEND_IP);
        resolved = true;
    }
    state_unlock();

    if (resolved) {
        return true;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        netif = esp_netif_get_default_netif();
    }
    if (netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.gw.addr != 0) {
        snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.gw));
        return true;
    }
    return false;
}

uint16_t get_backend_port(void) {
    char port_str[sizeof(g_backend_port)] = {0};
    uint16_t port = 0;

    state_lock();
    copy_string(port_str, sizeof(port_str),
                g_backend_port[0] != '\0' ? g_backend_port : CONFIG_BACKEND_PORT);
    state_unlock();

    if (!parse_backend_port(port_str, &port)) {
        port = 8080;
    }
    return port;
}

void wifi_notify_backend_failure(void) {
    backend_request_discovery();
}

bool wifi_camera_stream_is_healthy(void) {
    TickType_t last;

    if (!s_stream_has_succeeded) {
        return false;
    }
    last = s_last_stream_ok_tick;
    return (TickType_t)(xTaskGetTickCount() - last) < pdMS_TO_TICKS(HTTP_TIMEOUT_MS);
}

bool wifi_nvs_load_credentials(void) {
    nvs_handle_t handle;
    char ssid[sizeof(g_wifi_ssid)] = {0};
    char pass[sizeof(g_wifi_pass)] = {0};
    char backend_ip[sizeof(g_backend_ip)] = {0};
    char backend_port[sizeof(g_backend_port)] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    size_t ip_len = sizeof(backend_ip);
    size_t port_len = sizeof(backend_port);
    esp_err_t err;
    esp_err_t err_ssid;
    esp_err_t err_pass;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err_ssid = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    err_pass = nvs_get_str(handle, "pass", pass, &pass_len);
    (void)nvs_get_str(handle, "backend_ip", backend_ip, &ip_len);
    (void)nvs_get_str(handle, "backend_port", backend_port, &port_len);
    nvs_close(handle);

    if (err_ssid != ESP_OK || !is_safe_ssid(ssid)) {
        return false;
    }

    if (err_pass != ESP_OK || !is_safe_password(pass) || pass[0] == '\0') {
        copy_string(pass, sizeof(pass), CONFIG_ESP_WIFI_PASSWORD);
    }
    if (!is_valid_ipv4_str(backend_ip)) {
        backend_ip[0] = '\0';
    }
    {
        uint16_t parsed_port = 0;
        if (!parse_backend_port(backend_port, &parsed_port)) {
            backend_port[0] = '\0';
        }
    }

    state_lock();
    copy_string(g_wifi_ssid, sizeof(g_wifi_ssid), ssid);
    copy_string(g_wifi_pass, sizeof(g_wifi_pass), pass);
    copy_string(g_backend_ip, sizeof(g_backend_ip), backend_ip);
    copy_string(g_backend_port, sizeof(g_backend_port), backend_port);
    state_unlock();

    ESP_LOGI(TAG, "Loaded credentials from NVS (SSID: %s, backend: %s:%s)",
             ssid, backend_ip, backend_port);
    return true;
}

bool wifi_nvs_save_full_config(const char *ssid, const char *pass,
                               const char *backend_ip, const char *backend_port) {
    nvs_handle_t handle;
    esp_err_t err;
    char resolved_pass[sizeof(g_wifi_pass)] = {0};
    uint16_t parsed_port = 0;

    if (!is_safe_ssid(ssid)) {
        return false;
    }
    if (backend_ip != NULL && backend_ip[0] != '\0' && !is_valid_ipv4_str(backend_ip)) {
        return false;
    }
    if (backend_port != NULL && backend_port[0] != '\0' &&
        !parse_backend_port(backend_port, &parsed_port)) {
        return false;
    }
    if (pass != NULL && pass[0] != '\0') {
        if (!is_safe_password(pass)) {
            return false;
        }
        copy_string(resolved_pass, sizeof(resolved_pass), pass);
    } else {
        copy_string(resolved_pass, sizeof(resolved_pass), CONFIG_ESP_WIFI_PASSWORD);
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK && resolved_pass[0] != '\0') {
        err = nvs_set_str(handle, "pass", resolved_pass);
    }
    if (err == ESP_OK && backend_ip != NULL && backend_ip[0] != '\0') {
        err = nvs_set_str(handle, "backend_ip", backend_ip);
    }
    if (err == ESP_OK && backend_port != NULL && backend_port[0] != '\0') {
        err = nvs_set_str(handle, "backend_port", backend_port);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved Wi-Fi configuration to NVS (SSID: %s)", ssid);
        return true;
    }
    ESP_LOGE(TAG, "Failed to save Wi-Fi configuration: %s", esp_err_to_name(err));
    return false;
}

bool wifi_nvs_save_credentials(const char *ssid, const char *pass) {
    return wifi_nvs_save_full_config(ssid, pass, NULL, NULL);
}

bool wifi_apply_runtime_credentials(const char *ssid, const char *pass,
                                    const char *backend_ip, const char *backend_port) {
    char new_ssid[sizeof(g_wifi_ssid)];
    char new_pass[sizeof(g_wifi_pass)];
    char new_ip[sizeof(g_backend_ip)];
    char new_port[sizeof(g_backend_port)];
    uint16_t parsed_port = 0;

    if (!is_safe_ssid(ssid)) {
        return false;
    }
    copy_string(new_ssid, sizeof(new_ssid), ssid);

    state_lock();
    copy_string(new_pass, sizeof(new_pass), g_wifi_pass);
    copy_string(new_ip, sizeof(new_ip), g_backend_ip);
    copy_string(new_port, sizeof(new_port), g_backend_port);
    state_unlock();

    if (pass != NULL && pass[0] != '\0') {
        if (!is_safe_password(pass)) {
            return false;
        }
        copy_string(new_pass, sizeof(new_pass), pass);
    } else if (CONFIG_ESP_WIFI_PASSWORD[0] != '\0') {
        copy_string(new_pass, sizeof(new_pass), CONFIG_ESP_WIFI_PASSWORD);
    }

    if (backend_ip != NULL && backend_ip[0] != '\0') {
        if (!is_valid_ipv4_str(backend_ip)) {
            return false;
        }
        copy_string(new_ip, sizeof(new_ip), backend_ip);
    }
    if (backend_port != NULL && backend_port[0] != '\0') {
        if (!parse_backend_port(backend_port, &parsed_port)) {
            return false;
        }
        snprintf(new_port, sizeof(new_port), "%u", (unsigned)parsed_port);
    }

    if (!wifi_nvs_save_full_config(new_ssid, new_pass, new_ip, new_port)) {
        ESP_LOGE(TAG, "Failed to persist Wi-Fi credentials to NVS");
        return false;
    }

    state_lock();
    copy_string(g_wifi_ssid, sizeof(g_wifi_ssid), new_ssid);
    copy_string(g_wifi_pass, sizeof(g_wifi_pass), new_pass);
    copy_string(g_backend_ip, sizeof(g_backend_ip), new_ip);
    copy_string(g_backend_port, sizeof(g_backend_port), new_port);
    s_discovered_backend_ip[0] = '\0';
    s_backend_discovery_pending = true;
    s_last_discovery_tick = 0;
    g_wifi_retry_num = 0;
    state_unlock();

    wifi_request_http_reset();

    if (!s_wifi_initialized) {
        return true;
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    copy_string((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), new_ssid);
    copy_string((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), new_pass);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = (new_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }

    (void)esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply Wi-Fi configuration");
        return false;
    }
    if (esp_wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect request failed; reconnect timer will retry");
    }
    lcd_update_http_status("USB WIFI SYNC...", false);
    return true;
}

void wifi_init_sta(void) {
    esp_err_t err;
    char ssid[sizeof(g_wifi_ssid)];
    char pass[sizeof(g_wifi_pass)];

    if (s_wifi_initialized) {
        return;
    }

    if (s_state_lock == NULL) {
        s_state_lock = xSemaphoreCreateMutex();
        if (s_state_lock == NULL) {
            ESP_LOGE(TAG_WIFI, "Failed to create state mutex");
        }
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_WIFI, "NVS init failed: %s", esp_err_to_name(err));
    }

    if (!wifi_nvs_load_credentials()) {
        if (is_safe_ssid(CONFIG_ESP_WIFI_SSID)) {
            state_lock();
            copy_string(g_wifi_ssid, sizeof(g_wifi_ssid), CONFIG_ESP_WIFI_SSID);
            copy_string(g_wifi_pass, sizeof(g_wifi_pass), CONFIG_ESP_WIFI_PASSWORD);
            state_unlock();
        } else {
            ESP_LOGW(TAG_WIFI, "NVS empty. Waiting for USB provisioning.");
        }
    }

    state_lock();
    if (g_wifi_pass[0] == '\0' && CONFIG_ESP_WIFI_PASSWORD[0] != '\0') {
        copy_string(g_wifi_pass, sizeof(g_wifi_pass), CONFIG_ESP_WIFI_PASSWORD);
    }
    if (g_backend_ip[0] == '\0' && is_valid_ipv4_str(CONFIG_BACKEND_IP)) {
        copy_string(g_backend_ip, sizeof(g_backend_ip), CONFIG_BACKEND_IP);
    }
    if (g_backend_port[0] == '\0') {
        uint16_t config_port = 0;
        if (parse_backend_port(CONFIG_BACKEND_PORT, &config_port)) {
            snprintf(g_backend_port, sizeof(g_backend_port), "%u", (unsigned)config_port);
        } else {
            copy_string(g_backend_port, sizeof(g_backend_port), "8080");
        }
    }
    s_discovered_backend_ip[0] = '\0';
    s_backend_discovery_pending = !backend_static_ip_available();
    s_last_discovery_tick = 0;
    copy_string(ssid, sizeof(ssid), g_wifi_ssid);
    copy_string(pass, sizeof(pass), g_wifi_pass);
    state_unlock();

    if (g_wifi_event_group == NULL) {
        g_wifi_event_group = xEventGroupCreate();
        if (g_wifi_event_group == NULL) {
            ESP_LOGE(TAG_WIFI, "Failed to create Wi-Fi event group");
            return;
        }
    }

    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = wifi_reconnect_timer_cb,
            .arg = NULL,
            .name = "wifi_reconnect",
        };
        if (esp_timer_create(&timer_args, &s_reconnect_timer) != ESP_OK) {
            ESP_LOGW(TAG_WIFI, "Failed to create reconnect timer");
            s_reconnect_timer = NULL;
        }
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_WIFI, "esp_netif_init failed: %s", esp_err_to_name(err));
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_WIFI, "Event loop init failed: %s", esp_err_to_name(err));
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG_WIFI, "Failed to create Wi-Fi station netif");
            return;
        }
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    copy_string((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid);
    copy_string((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), pass);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_LOGI(TAG_WIFI, "Starting STA (SSID: '%s', password length: %d)",
             ssid, (int)strlen(pass));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Failed to set station mode: %s", esp_err_to_name(err));
        return;
    }

    if (ssid[0] != '\0') {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_WIFI, "Failed to set station config: %s", esp_err_to_name(err));
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return;
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    s_wifi_initialized = true;
}

esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port,
                                    telemetry_data_t *data) {
    char url[URL_BUFFER_SIZE];
    char device_id[DEVICE_ID_MAX_LEN];
    char firmware_version[FW_VERSION_MAX_LEN];
    char local_ip[24] = {0};
    telemetry_data_t meta;
    cJSON *root;
    char *json_string;
    esp_http_client_handle_t client;
    esp_err_t err;

    if (backend_ip == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!format_url(url, sizeof(url), backend_ip, port, BACKEND_ENDPOINT)) {
        return ESP_ERR_INVALID_ARG;
    }

    init_device_metadata(&meta);
    copy_string(device_id, sizeof(device_id),
                data->device_id[0] != '\0' ? data->device_id : meta.device_id);
    copy_string(firmware_version, sizeof(firmware_version),
                data->firmware_version[0] != '\0' ? data->firmware_version : meta.firmware_version);

    float temperature = data->temperature > 0.0f ? data->temperature : get_esp32_cpu_temperature();
    uint8_t cpu_load = data->current_cpu_load > 0 ? data->current_cpu_load : get_cpu_usage_percentage();
    int64_t timestamp = data->timestamp != 0 ? data->timestamp : (int64_t)time(NULL);
    const char *camera_model = camera_get_sensor_name();
    if (camera_model == NULL || camera_model[0] == '\0') {
        camera_model = "UNKNOWN";
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "firmware_version", firmware_version);
    cJSON_AddNumberToObject(root, "timestamp", (double)timestamp);
    cJSON_AddNumberToObject(root, "temperature", (double)temperature);
    cJSON_AddNumberToObject(root, "cpu_load", (double)cpu_load);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddStringToObject(root, "camera_model", camera_model);

    json_string = cJSON_PrintUnformatted(root);
    if (json_string == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    if (s_telemetry_reset_pending) {
        s_telemetry_reset_pending = false;
        wifi_reset_telemetry_client();
    }

    if (s_telemetry_client == NULL || strcmp(s_telemetry_url, url) != 0) {
        wifi_reset_telemetry_client();

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .keep_alive_enable = true,
        };
        s_telemetry_client = esp_http_client_init(&config);
        if (s_telemetry_client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize telemetry HTTP client");
            cJSON_Delete(root);
            cJSON_free(json_string);
            return ESP_FAIL;
        }
        copy_string(s_telemetry_url, sizeof(s_telemetry_url), url);
    }

    client = s_telemetry_client;
    (void)esp_http_client_set_header(client, "Content-Type", "application/json");
    if (wifi_get_local_ip(local_ip, sizeof(local_ip))) {
        (void)esp_http_client_set_header(client, "X-Device-IP", local_ip);
    }
    (void)esp_http_client_set_post_field(client, json_string, (int)strlen(json_string));

    ESP_LOGI(TAG, "POST %s (device=%s, cpu=%u%%, heap=%u)",
             url, device_id, (unsigned)cpu_load, (unsigned)esp_get_free_heap_size());

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "Telemetry accepted (HTTP %d)", status);
            char status_buf[32];
            snprintf(status_buf, sizeof(status_buf), "HTTP %d (OK)", status);
            lcd_update_http_status(status_buf, false);
            backend_mark_reachable();
        } else {
            ESP_LOGW(TAG, "Telemetry rejected (HTTP %d)", status);
            lcd_update_http_status("HTTP: REJECTED", true);
            (void)esp_http_client_close(client);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Telemetry failed: %s (errno %d)",
                 esp_err_to_name(err), esp_http_client_get_errno(client));
        lcd_update_http_status("HTTP: TIMEOUT", true);
        (void)esp_http_client_close(client);
        backend_request_discovery();
    }

    cJSON_Delete(root);
    cJSON_free(json_string);
    return err;
}

esp_err_t send_camera_frame_to_backend(const char *backend_ip, uint16_t port,
                                       const char *device_id) {
    static camera_frame_buffer_t tx_frame = {0};
    char url[URL_BUFFER_SIZE];
    char frame_id_str[16];
    char temp_str[16];
    char load_str[16];
    esp_http_client_handle_t client;
    esp_err_t err;

    if (backend_ip == NULL || device_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_camera_streaming()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!format_url(url, sizeof(url), backend_ip, port, CAMERA_FRAME_ENDPOINT)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tx_frame.data == NULL) {
        tx_frame.capacity = INITIAL_BUF_CAPACITY;
        tx_frame.data = (uint8_t *)heap_caps_malloc(tx_frame.capacity,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tx_frame.data == NULL) {
            tx_frame.data = (uint8_t *)malloc(tx_frame.capacity);
        }
        if (tx_frame.data == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (camera_copy_latest_frame(&tx_frame) != ESP_OK || tx_frame.header.buffer_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_stream_reset_pending) {
        s_stream_reset_pending = false;
        wifi_reset_stream_client();
    }

    if (s_stream_client == NULL || strcmp(s_stream_url, url) != 0) {
        wifi_reset_stream_client();

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .buffer_size_tx = STREAM_TX_BUFFER_SIZE,
            .keep_alive_enable = true,
        };
        s_stream_client = esp_http_client_init(&config);
        if (s_stream_client == NULL) {
            if (s_stream_failures < UINT8_MAX) {
                s_stream_failures++;
            }
            return ESP_FAIL;
        }
        copy_string(s_stream_url, sizeof(s_stream_url), url);
    }

    client = s_stream_client;

    snprintf(frame_id_str, sizeof(frame_id_str), "%lu",
             (unsigned long)tx_frame.header.frame_id);
    snprintf(temp_str, sizeof(temp_str), "%.2f", get_esp32_cpu_temperature());
    snprintf(load_str, sizeof(load_str), "%u", (unsigned)get_cpu_usage_percentage());

    (void)esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    (void)esp_http_client_set_header(client, "Connection", "keep-alive");
    (void)esp_http_client_set_header(client, "X-Device-ID", device_id);
    (void)esp_http_client_set_header(client, "X-Frame-ID", frame_id_str);
    (void)esp_http_client_set_header(client, "X-ESP-Temp", temp_str);
    (void)esp_http_client_set_header(client, "X-ESP-CPU-Load", load_str);
    (void)esp_http_client_set_header(client, "X-Camera-Model", camera_get_sensor_name());
    (void)esp_http_client_set_post_field(client, (const char *)tx_frame.data,
                                         (int)tx_frame.header.buffer_len);

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            s_last_stream_ok_tick = xTaskGetTickCount();
            s_stream_has_succeeded = true;
            s_stream_failures = 0;
            backend_mark_reachable();
        } else {
            static TickType_t last_reject_log = 0;
            TickType_t now = xTaskGetTickCount();
            if (last_reject_log == 0 ||
                (TickType_t)(now - last_reject_log) >= pdMS_TO_TICKS(5000)) {
                ESP_LOGW(TAG, "Frame rejected (HTTP %d)", status);
                last_reject_log = now;
            }
            (void)esp_http_client_close(client);
            if (s_stream_failures < UINT8_MAX) {
                s_stream_failures++;
            }
            err = ESP_FAIL;
        }
    } else {
        static TickType_t last_error_log = 0;
        TickType_t now = xTaskGetTickCount();
        int socket_errno = esp_http_client_get_errno(client);

        if (last_error_log == 0 ||
            (TickType_t)(now - last_error_log) >= pdMS_TO_TICKS(5000)) {
            ESP_LOGW(TAG, "Frame upload failed: %s (errno %d)",
                     esp_err_to_name(err), socket_errno);
            last_error_log = now;
        }
        (void)esp_http_client_close(client);
        if (s_stream_failures < UINT8_MAX) {
            s_stream_failures++;
        }
        if (s_stream_failures >= STREAM_FAILURE_RESET_THRESHOLD) {
            wifi_reset_stream_client();
            backend_request_discovery();
        }
    }

    return err;
}

void http_stream_sender_task(void *pvParameters) {
    const TickType_t frame_interval_ticks = pdMS_TO_TICKS(1000 / STREAM_FPS);
    char backend_ip[32];
    char device_id[DEVICE_ID_MAX_LEN];

    (void)pvParameters;

    ESP_LOGI(TAG, "HTTP stream sender started (target %d FPS)", STREAM_FPS);

    for (;;) {
        TickType_t start_tick = xTaskGetTickCount();

        if (!wifi_is_connected()) {
            if (s_stream_reset_pending) {
                s_stream_reset_pending = false;
                wifi_reset_stream_client();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (s_stream_reset_pending) {
            s_stream_reset_pending = false;
            wifi_reset_stream_client();
        }

        if (!is_camera_streaming()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        get_internal_device_id(device_id, sizeof(device_id));
        if (device_id[0] == '\0') {
            copy_string(device_id, sizeof(device_id), DEVICE_ID);
        }

        uint8_t failures_before = s_stream_failures;
        (void)send_camera_frame_to_backend(backend_ip, get_backend_port(), device_id);

        if (s_stream_failures > failures_before) {
            uint32_t shift = s_stream_failures > 4 ? 4U : (uint32_t)(s_stream_failures - 1U);
            vTaskDelay(pdMS_TO_TICKS(250U << shift));
            continue;
        }

        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks < frame_interval_ticks) {
            vTaskDelay(frame_interval_ticks - elapsed_ticks);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void WiFi_Server_Task(void *pvParameters) {
    QueueHandle_t data_queue = (QueueHandle_t)pvParameters;
    telemetry_data_t telemetry;

    if (data_queue == NULL) {
        ESP_LOGE(TAG, "Telemetry queue is NULL");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        char backend_ip[32];
        uint16_t backend_port;

        if (xQueueReceive(data_queue, &telemetry, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!wifi_is_connected()) {
            continue;
        }
        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        backend_port = get_backend_port();
        if (send_telemetry_to_backend(backend_ip, backend_port, &telemetry) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static void usb_send_ack(const char *status) {
    telemetry_data_t meta;
    char ssid[sizeof(g_wifi_ssid)];
    char backend_ip[sizeof(g_backend_ip)];
    uint16_t backend_port;
    float cpu_temp;
    uint8_t cpu_load;
    bool connected;
    cJSON *ack;
    char *json_string;

    init_device_metadata(&meta);

    state_lock();
    copy_string(ssid, sizeof(ssid), g_wifi_ssid);
    copy_string(backend_ip, sizeof(backend_ip), g_backend_ip);
    state_unlock();

    backend_port = get_backend_port();
    cpu_temp = get_esp32_cpu_temperature();
    cpu_load = get_cpu_usage_percentage();
    connected = wifi_is_connected();

    ack = cJSON_CreateObject();
    if (ack == NULL) {
        return;
    }

    cJSON_AddStringToObject(ack, "status", status);
    cJSON_AddStringToObject(ack, "device_id", meta.device_id);
    cJSON_AddStringToObject(ack, "firmware_version", meta.firmware_version);
    cJSON_AddBoolToObject(ack, "wifi_connected", connected);
    cJSON_AddStringToObject(ack, "ssid", ssid);
    cJSON_AddStringToObject(ack, "backend_ip", backend_ip);
    cJSON_AddNumberToObject(ack, "backend_port", (double)backend_port);
    cJSON_AddNumberToObject(ack, "temperature", (double)cpu_temp);
    cJSON_AddNumberToObject(ack, "cpu_temperature", (double)cpu_temp);
    cJSON_AddNumberToObject(ack, "cpu_load", (double)cpu_load);

    json_string = cJSON_PrintUnformatted(ack);
    if (json_string != NULL) {
        printf("BAWAL_ACK:%s\n", json_string);
        fflush(stdout);
        cJSON_free(json_string);
    }
    cJSON_Delete(ack);
}

void usb_serial_provisioning_task(void *pvParameters) {
    char line[256];
    size_t index = 0;

    (void)pvParameters;

    for (;;) {
        int ch = fgetc(stdin);

        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            char *json_start;

            if (index == 0) {
                continue;
            }
            line[index] = '\0';
            index = 0;

            json_start = strchr(line, '{');
            if (json_start == NULL) {
                continue;
            }

            cJSON *cmd_json = cJSON_Parse(json_start);
            if (cmd_json == NULL) {
                continue;
            }

            cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "cmd");
            if (cJSON_IsString(cmd_item) && cmd_item->valuestring != NULL) {
                if (strcmp(cmd_item->valuestring, "set_wifi") == 0 ||
                    strcmp(cmd_item->valuestring, "provision_wifi") == 0) {
                    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "ssid");
                    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "pass");
                    cJSON *ip_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "backend_ip");
                    cJSON *port_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "backend_port");

                    const char *new_ssid = (cJSON_IsString(ssid_item) &&
                                            ssid_item->valuestring != NULL) ?
                                               ssid_item->valuestring : "";
                    const char *new_pass = (cJSON_IsString(pass_item) &&
                                            pass_item->valuestring != NULL) ?
                                               pass_item->valuestring : "";
                    const char *new_ip = (cJSON_IsString(ip_item) &&
                                          ip_item->valuestring != NULL) ?
                                             ip_item->valuestring : "";
                    const char *new_port = (cJSON_IsString(port_item) &&
                                            port_item->valuestring != NULL) ?
                                               port_item->valuestring : "";

                    bool ok = wifi_apply_runtime_credentials(new_ssid, new_pass,
                                                             new_ip, new_port);
                    if (ok && g_wifi_event_group != NULL) {
                        (void)xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT,
                                                  pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
                    }
                    usb_send_ack(ok ? "wifi_provisioned" : "error");
                } else if (strcmp(cmd_item->valuestring, "status") == 0 ||
                           strcmp(cmd_item->valuestring, "get_status") == 0) {
                    usb_send_ack("ok");
                }
            }

            cJSON_Delete(cmd_json);
        } else if (index < sizeof(line) - 1) {
            if ((unsigned char)ch >= 0x20 || ch == '\t') {
                line[index++] = (char)ch;
            }
        } else {
            index = 0;
        }
    }
}
