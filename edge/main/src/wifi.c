#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_event.h"
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
#include "esp_camera.h"

#define HTTP_TIMEOUT_MS 1500
#define DISCOVERY_RETRY_MS 5000
#define DISCOVERY_TOTAL_TIMEOUT_MS 700

EventGroupHandle_t g_wifi_event_group = NULL;

char g_wifi_ssid[32] = {0};
char g_wifi_pass[64] = {0};
char g_backend_ip[32] = {0};
char g_backend_port[8] = {0};

static const char *TAG = "WIFI";
static bool s_wifi_initialized = false;
static int s_retry_num = 0;
static char s_discovered_backend_ip[32] = {0};
static TickType_t s_last_discovery_tick = 0;

static int s_frame_sock = -1;
static char s_frame_ip[32] = {0};
static uint16_t s_frame_port = 0;
static volatile TickType_t s_last_rx_tick = 0;
static volatile float s_cached_temperature = 0.0f;
static volatile uint8_t s_cached_cpu_load = 0;

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
    unsigned int a = 0, b = 0, c = 0, d = 0;
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
    size_t len, i;

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
    size_t len, i;

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

static bool format_url(char *out_url, size_t max_len, const char *ip,
                       uint16_t port, const char *path) {
    int written;

    if (out_url == NULL || max_len == 0 || ip == NULL || path == NULL) {
        return false;
    }
    written = snprintf(out_url, max_len, "http://%s:%u%s", ip, (unsigned)port, path);
    return written > 0 && (size_t)written < max_len;
}

static bool wifi_is_connected(void) {
    if (g_wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

static bool wifi_discover_backend_ip(char *out_ip, size_t max_len) {
    int sock;
    int broadcast_enabled = 1;
    struct timeval recv_timeout = {0};
    struct sockaddr_in dest;
    esp_netif_t *netif;
    esp_netif_ip_info_t ip_info;
    const char *probe = BACKEND_DISCOVERY_MAGIC;
    bool sent = false;
    bool found = false;
    TickType_t deadline;
    char response[192];

    if (out_ip == NULL || max_len == 0) {
        return false;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
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

    if (sendto(sock, probe, strlen(probe), 0, (struct sockaddr *)&dest, sizeof(dest)) >= 0) {
        sent = true;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        uint32_t mask = ntohl(ip_info.netmask.addr);
        if (mask != 0 && mask != 0xFFFFFFFFU) {
            uint32_t broadcast = (ntohl(ip_info.ip.addr) & mask) | ~mask;
            dest.sin_addr.s_addr = htonl(broadcast);
            if (sendto(sock, probe, strlen(probe), 0, (struct sockaddr *)&dest, sizeof(dest)) >= 0) {
                sent = true;
            }
        }
    }

    if (!sent) {
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
    return found;
}

bool get_backend_ip(char *out_ip, size_t max_len) {
    if (out_ip == NULL || max_len == 0) {
        return false;
    }

    if (is_valid_ipv4_str(g_backend_ip)) {
        copy_string(out_ip, max_len, g_backend_ip);
        return true;
    }
    if (is_valid_ipv4_str(CONFIG_BACKEND_IP)) {
        copy_string(out_ip, max_len, CONFIG_BACKEND_IP);
        return true;
    }
    if (is_valid_ipv4_str(s_discovered_backend_ip)) {
        copy_string(out_ip, max_len, s_discovered_backend_ip);
        return true;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_last_discovery_tick == 0 ||
        (TickType_t)(now - s_last_discovery_tick) >= pdMS_TO_TICKS(DISCOVERY_RETRY_MS)) {
        char discovered[32] = {0};
        s_last_discovery_tick = now;
        if (wifi_discover_backend_ip(discovered, sizeof(discovered))) {
            copy_string(s_discovered_backend_ip, sizeof(s_discovered_backend_ip), discovered);
            copy_string(out_ip, max_len, discovered);
            ESP_LOGI(TAG, "Discovered backend at %s", discovered);
            return true;
        }
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        netif = esp_netif_get_default_netif();
    }
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.gw.addr != 0) {
            snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.gw));
            return true;
        }
    }
    return false;
}

uint16_t get_backend_port(void) {
    uint16_t port = 0;
    const char *port_str = g_backend_port[0] != '\0' ? g_backend_port : CONFIG_BACKEND_PORT;

    if (!parse_backend_port(port_str, &port)) {
        port = 8080;
    }
    return port;
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
    uint16_t parsed_port = 0;
    if (!parse_backend_port(backend_port, &parsed_port)) {
        backend_port[0] = '\0';
    }

    copy_string(g_wifi_ssid, sizeof(g_wifi_ssid), ssid);
    copy_string(g_wifi_pass, sizeof(g_wifi_pass), pass);
    copy_string(g_backend_ip, sizeof(g_backend_ip), backend_ip);
    copy_string(g_backend_port, sizeof(g_backend_port), backend_port);

    ESP_LOGI(TAG, "Loaded NVS config (SSID: %s, backend: %s:%s)",
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
        ESP_LOGI(TAG, "Saved Wi-Fi config to NVS (SSID: %s)", ssid);
        return true;
    }
    ESP_LOGE(TAG, "Failed to save Wi-Fi config: %s", esp_err_to_name(err));
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
    copy_string(new_pass, sizeof(new_pass), g_wifi_pass);
    copy_string(new_ip, sizeof(new_ip), g_backend_ip);
    copy_string(new_port, sizeof(new_port), g_backend_port);

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
        return false;
    }

    copy_string(g_wifi_ssid, sizeof(g_wifi_ssid), new_ssid);
    copy_string(g_wifi_pass, sizeof(g_wifi_pass), new_pass);
    copy_string(g_backend_ip, sizeof(g_backend_ip), new_ip);
    copy_string(g_backend_port, sizeof(g_backend_port), new_port);
    s_discovered_backend_ip[0] = '\0';
    s_last_discovery_tick = 0;

    if (!s_wifi_initialized) {
        return true;
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    copy_string((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), new_ssid);
    copy_string((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), new_pass);
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = (new_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    (void)esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        return false;
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    if (esp_wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect request failed");
    }
    lcd_update_http_status("USB WIFI SYNC...", false);
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_retry_num = 0;
        if (g_wifi_event_group != NULL) {
            (void)xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }
        if (g_wifi_ssid[0] != '\0') {
            (void)esp_wifi_set_ps(WIFI_PS_NONE);
            ESP_LOGI(TAG, "Connecting to AP '%s'...", g_wifi_ssid);
            lcd_update_http_status("WIFI CONNECTING...", false);
            (void)esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "No SSID configured. Waiting for USB provisioning.");
            lcd_update_http_status("WAIT USB WIFI...", false);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        lcd_update_http_status("DHCP REQUEST...", false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;

        if (g_wifi_event_group != NULL) {
            (void)xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(false, NULL);
        lcd_update_http_status("WIFI LOST", true);

        if (g_wifi_ssid[0] == '\0') {
            return;
        }
        if (s_retry_num < 5) {
            s_retry_num++;
            ESP_LOGW(TAG, "Disconnected (reason %d); retry %d/5 in 1000 ms",
                     dis != NULL ? dis->reason : -1, s_retry_num);
            vTaskDelay(pdMS_TO_TICKS(1000));
            (void)esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Disconnected (reason %d); retry limit reached",
                     dis != NULL ? dis->reason : -1);
            if (g_wifi_event_group != NULL) {
                (void)xEventGroupSetBits(g_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[24];

        if (event == NULL) {
            return;
        }
        s_retry_num = 0;
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        (void)esp_wifi_set_inactive_time(WIFI_IF_STA, 0);
        wifi_ps_type_t ps_mode = WIFI_PS_MIN_MODEM;
        (void)esp_wifi_get_ps(&ps_mode);
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Successfully connected! Assigned IP: %s (RSSI %d dBm, PS %d, auth %d, 11n %d, 11ax %d)",
                     ip_str, ap_info.rssi, (int)ps_mode, (int)ap_info.authmode,
                     ap_info.phy_11n, ap_info.phy_11ax);
        } else {
            ESP_LOGI(TAG, "Successfully connected! Assigned IP: %s", ip_str);
        }

        if (g_wifi_event_group != NULL) {
            (void)xEventGroupClearBits(g_wifi_event_group, WIFI_FAIL_BIT);
            (void)xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(true, ip_str);
        lcd_update_http_status("WIFI READY", false);
    }
}

void wifi_init_sta(void) {
    esp_err_t err;
    char ssid[sizeof(g_wifi_ssid)];
    char pass[sizeof(g_wifi_pass)];

    if (s_wifi_initialized) {
        return;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

    if (!wifi_nvs_load_credentials()) {
        if (is_safe_ssid(CONFIG_ESP_WIFI_SSID)) {
            copy_string(g_wifi_ssid, sizeof(g_wifi_ssid), CONFIG_ESP_WIFI_SSID);
            copy_string(g_wifi_pass, sizeof(g_wifi_pass), CONFIG_ESP_WIFI_PASSWORD);
        } else {
            ESP_LOGW(TAG, "NVS empty. Waiting for USB provisioning.");
        }
    }

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
    copy_string(ssid, sizeof(ssid), g_wifi_ssid);
    copy_string(pass, sizeof(pass), g_wifi_pass);

    if (g_wifi_event_group == NULL) {
        g_wifi_event_group = xEventGroupCreate();
        if (g_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
            return;
        }
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop init failed: %s", esp_err_to_name(err));
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "Failed to create Wi-Fi station netif");
            return;
        }
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    copy_string((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid);
    copy_string((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), pass);
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_LOGI(TAG, "Starting STA (SSID: '%s', password length: %d)",
             ssid, (int)strlen(pass));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set station mode: %s", esp_err_to_name(err));
        return;
    }

    if (ssid[0] != '\0') {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set station config: %s", esp_err_to_name(err));
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return;
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_wifi_set_inactive_time(WIFI_IF_STA, 0);
    (void)esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
    (void)esp_wifi_set_max_tx_power(68);
    s_wifi_initialized = true;
}

esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port,
                                    telemetry_data_t *data) {
    char url[URL_BUFFER_SIZE];
    char device_id[DEVICE_ID_MAX_LEN];
    char firmware_version[FW_VERSION_MAX_LEN];
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
    wifi_ap_record_t ap_info;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
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
    cJSON_AddStringToObject(root, "camera_model", camera_model != NULL ? camera_model : "UNKNOWN");
    cJSON_AddNumberToObject(root, "rssi", rssi);

    json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_string == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    client = esp_http_client_init(&config);
    if (client == NULL) {
        cJSON_free(json_string);
        return ESP_FAIL;
    }

    (void)esp_http_client_set_header(client, "Content-Type", "application/json");
    (void)esp_http_client_set_post_field(client, json_string, (int)strlen(json_string));

    ESP_LOGI(TAG, "POST %s (device=%s, cpu=%u%%, heap=%u)",
             url, device_id, (unsigned)cpu_load, (unsigned)esp_get_free_heap_size());

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            char status_buf[32];
            snprintf(status_buf, sizeof(status_buf), "HTTP %d (OK)", status);
            lcd_update_http_status(status_buf, false);
        } else {
            ESP_LOGW(TAG, "Telemetry rejected (HTTP %d)", status);
            lcd_update_http_status("HTTP: REJECTED", true);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "Telemetry failed: %s (errno %d)",
                 esp_err_to_name(err), esp_http_client_get_errno(client));
        lcd_update_http_status("HTTP: TIMEOUT", true);
    }

    esp_http_client_cleanup(client);
    cJSON_free(json_string);
    return err;
}

static void frame_socket_close(void) {
    if (s_frame_sock >= 0) {
        close(s_frame_sock);
        s_frame_sock = -1;
    }
    s_frame_ip[0] = '\0';
    s_frame_port = 0;
}

static int frame_socket_open(const char *ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int nodelay = 1;
    (void)setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    int flags = fcntl(sock, F_GETFL, 0);
    (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = inet_addr(ip);

    int connected = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (connected != 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    if (connected != 0) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        struct timeval connect_timeout = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(sock + 1, NULL, &writefds, NULL, &connect_timeout);
        if (ready <= 0) {
            close(sock);
            return -1;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
            close(sock);
            return -1;
        }
    }
    (void)fcntl(sock, F_SETFL, flags);

    s_frame_sock = sock;
    copy_string(s_frame_ip, sizeof(s_frame_ip), ip);
    s_frame_port = port;
    s_last_rx_tick = xTaskGetTickCount();
    return 0;
}

static bool frame_send_all(int sock, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int written = send(sock, data + sent, len - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += (size_t)written;
    }
    return true;
}

static void frame_drain_responses(uint32_t *ok_count) {
    char response[512];
    for (;;) {
        int received = recv(s_frame_sock, response, sizeof(response) - 1, MSG_DONTWAIT);
        if (received > 0) {
            s_last_rx_tick = xTaskGetTickCount();
            response[received] = '\0';
            if (strstr(response, "HTTP/1.1 200") != NULL || strstr(response, "HTTP/1.0 200") != NULL) {
                (*ok_count)++;
            }
            continue;
        }
        if (received == 0) {
            frame_socket_close();
        }
        break;
    }
}

void http_stream_sender_task(void *pvParameters) {
    const TickType_t frame_interval_ticks = pdMS_TO_TICKS(100);
    char backend_ip[32];
    char device_id[DEVICE_ID_MAX_LEN];
    telemetry_data_t meta;
    uint32_t frame_id = 0;
    uint32_t timing_frames = 0;
    uint32_t http_ok = 0;
    int64_t capture_us_total = 0;
    int64_t send_us_total = 0;
    TickType_t last_error_log = 0;

    (void)pvParameters;

    init_device_metadata(&meta);
    copy_string(device_id, sizeof(device_id), meta.device_id);
    if (device_id[0] == '\0') {
        copy_string(device_id, sizeof(device_id), "ESP32-S3");
    }

    ESP_LOGI(TAG, "HTTP stream sender started (100 ms pacing, device=%s)", device_id);

    for (;;) {
        TickType_t start_tick = xTaskGetTickCount();

        if (g_wifi_event_group == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        (void)xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT,
                                  pdFALSE, pdTRUE, portMAX_DELAY);
        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint16_t backend_port = get_backend_port();
        if (s_frame_sock < 0 || strcmp(s_frame_ip, backend_ip) != 0 ||
            s_frame_port != backend_port) {
            frame_socket_close();
            if (frame_socket_open(backend_ip, backend_port) != 0) {
                TickType_t now = xTaskGetTickCount();
                if (last_error_log == 0 ||
                    (TickType_t)(now - last_error_log) >= pdMS_TO_TICKS(5000)) {
                    ESP_LOGW(TAG, "Frame socket connect failed");
                    last_error_log = now;
                }
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            ESP_LOGI(TAG, "Frame socket -> %s:%u", backend_ip, (unsigned)backend_port);
        }

        int64_t capture_start_us = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        int64_t capture_done_us = esp_timer_get_time();
        if (fb == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (fb->format != PIXFORMAT_JPEG || fb->len < 2 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        frame_id++;
        const int jpeg_len = (int)fb->len;
        char header[512];
        int header_len = snprintf(header, sizeof(header),
                                  "POST /api/v1/camera/frame HTTP/1.1\r\n"
                                  "Host: %s:%u\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %d\r\n"
                                  "X-Device-ID: %s\r\n"
                                  "X-Frame-ID: %lu\r\n"
                                  "X-ESP-Temp: %.2f\r\n"
                                  "X-ESP-CPU-Load: %u\r\n"
                                  "X-Camera-Model: %s\r\n"
                                  "Connection: keep-alive\r\n"
                                  "\r\n",
                                  backend_ip, (unsigned)backend_port, jpeg_len,
                                  device_id, (unsigned long)frame_id,
                                  (double)s_cached_temperature,
                                  (unsigned)s_cached_cpu_load,
                                  camera_get_sensor_name());

        bool sent = header_len > 0 && (size_t)header_len < sizeof(header) &&
                    frame_send_all(s_frame_sock, (const uint8_t *)header, (size_t)header_len) &&
                    frame_send_all(s_frame_sock, fb->buf, (size_t)jpeg_len);
        int64_t send_done_us = esp_timer_get_time();
        esp_camera_fb_return(fb);

        if (!sent) {
            TickType_t now = xTaskGetTickCount();
            if (last_error_log == 0 ||
                (TickType_t)(now - last_error_log) >= pdMS_TO_TICKS(5000)) {
                ESP_LOGW(TAG, "Frame send failed (errno %d)", errno);
                last_error_log = now;
            }
            frame_socket_close();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        frame_drain_responses(&http_ok);
        if (frame_id > 5 && (TickType_t)(xTaskGetTickCount() - s_last_rx_tick) > pdMS_TO_TICKS(2000)) {
            ESP_LOGW(TAG, "Frame socket stalled for >2 s; reconnecting");
            frame_socket_close();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        capture_us_total += capture_done_us - capture_start_us;
        send_us_total += send_done_us - capture_done_us;
        timing_frames++;
        if (frame_id == 1) {
            ESP_LOGI(TAG, "First frame sent (%d B)", jpeg_len);
        }
        if (timing_frames >= 30) {
            ESP_LOGI(TAG, "avg capture %.1f ms, send %.1f ms, HTTP200 %lu, jpeg %d B",
                     (double)capture_us_total / timing_frames / 1000.0,
                     (double)send_us_total / timing_frames / 1000.0,
                     (unsigned long)http_ok, jpeg_len);
            timing_frames = 0;
            capture_us_total = 0;
            send_us_total = 0;
        }

        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks < frame_interval_ticks) {
            vTaskDelay(frame_interval_ticks - elapsed_ticks);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void WiFi_Server_Task(void *pvParameters) {
    telemetry_data_t telemetry;

    (void)pvParameters;
    init_device_metadata(&telemetry);

    for (;;) {
        char backend_ip[32];

        if (g_wifi_event_group == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        (void)xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT,
                                  pdFALSE, pdTRUE, portMAX_DELAY);
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        telemetry.timestamp = (int64_t)time(NULL);
        telemetry.temperature = get_esp32_cpu_temperature();
        telemetry.current_cpu_load = get_cpu_usage_percentage();
        s_cached_temperature = telemetry.temperature;
        s_cached_cpu_load = telemetry.current_cpu_load;

        printf("BAWAL_TELEMETRY:{\"device_id\":\"%s\",\"firmware_version\":\"%s\","
               "\"temperature\":%.2f,\"cpu_load\":%u}\n",
               telemetry.device_id, telemetry.firmware_version,
               telemetry.temperature, telemetry.current_cpu_load);
        fflush(stdout);

        if (!camera_task_is_ready() && get_backend_ip(backend_ip, sizeof(backend_ip))) {
            (void)send_telemetry_to_backend(backend_ip, get_backend_port(), &telemetry);
        }
        lcd_update_telemetry_display(telemetry.temperature, telemetry.current_cpu_load);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void usb_send_ack(const char *status) {
    telemetry_data_t meta;
    uint16_t backend_port;
    float cpu_temp;
    uint8_t cpu_load;
    bool connected;
    cJSON *ack;
    char *json_string;

    init_device_metadata(&meta);
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
    cJSON_AddStringToObject(ack, "ssid", g_wifi_ssid);
    cJSON_AddStringToObject(ack, "backend_ip", g_backend_ip);
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
