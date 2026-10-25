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

#define DISCOVERY_BACKOFF_MIN_MS 1000
#define DISCOVERY_BACKOFF_MAX_MS 30000
#define DISCOVERY_REFRESH_MS 30000
#define MJPEG_CLIENT_LIMIT 3
#define MJPEG_FRAME_CAPACITY INITIAL_BUF_CAPACITY
#define MJPEG_TARGET_CYCLE_US 66000
#define MJPEG_IDLE_TIMEOUT_US (10LL * 1000LL * 1000LL)

EventGroupHandle_t g_wifi_event_group = NULL;
int g_wifi_retry_num = 0;
char g_wifi_ssid[32] = {0};
char g_wifi_pass[64] = {0};
char g_backend_ip[32] = {0};
char g_backend_port[8] = "8080";

static char s_discovered_ip[32] = {0};
static bool s_nvs_wifi_pass_key_present = false;
static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_wifi_initialized = false;
static volatile bool s_provisioning_active = false;
static httpd_handle_t s_http_server = NULL;
static atomic_int s_mjpeg_clients = 0;

void wifi_notify_stream_frame_ready(void) {
    /* Camera capture now feeds the pull-based HTTP stream directly. */
}

static void str_trim_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' || src[len - 1] == '\n')) {
        len--;
    }
    if (len >= dst_sz) len = dst_sz - 1;
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

static bool is_docker_range(const char *ip) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    return a == 172 && b >= 16 && b <= 31;
}

static bool is_valid_lan_ip(const char *ip) {
    if (!is_valid_ipv4(ip)) return false;

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) return false;
    uint32_t host = ntohl(addr.s_addr);
    uint8_t a = (host >> 24) & 0xFF;
    uint8_t b = (host >> 16) & 0xFF;
    if (a == 0 || a == 127) return false;
    if (a == 169 && b == 254) return false;

    if (is_same_subnet_ip(ip)) return true;

    esp_netif_ip_info_t info;
    if (!get_sta_ip_info(&info)) return true;

    return !is_docker_range(ip);
}

bool wifi_nvs_load_credentials(void) {
    s_nvs_wifi_pass_key_present = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        err = nvs_open("wifi_config", NVS_READONLY, &handle);
        if (err != ESP_OK) return false;
    }

    size_t sz = sizeof(g_wifi_ssid);
    if (nvs_get_str(handle, NVS_KEY_WIFI_SSID, g_wifi_ssid, &sz) != ESP_OK) {
        sz = sizeof(g_wifi_ssid);
        (void)nvs_get_str(handle, "ssid", g_wifi_ssid, &sz);
    }

    sz = sizeof(g_wifi_pass);
    if (nvs_get_str(handle, NVS_KEY_WIFI_PASS, g_wifi_pass, &sz) == ESP_OK) {
        s_nvs_wifi_pass_key_present = true;
    } else {
        sz = sizeof(g_wifi_pass);
        if (nvs_get_str(handle, "pass", g_wifi_pass, &sz) == ESP_OK) {
            s_nvs_wifi_pass_key_present = true;
        }
    }

    sz = sizeof(g_backend_ip);
    (void)nvs_get_str(handle, NVS_KEY_BACKEND_IP, g_backend_ip, &sz);

    uint16_t port_u16 = 8080;
    if (nvs_get_u16(handle, NVS_KEY_BACKEND_PORT, &port_u16) == ESP_OK && port_u16 > 0) {
        snprintf(g_backend_port, sizeof(g_backend_port), "%u", port_u16);
    } else {
        sz = sizeof(g_backend_port);
        if (nvs_get_str(handle, "backend_port", g_backend_port, &sz) != ESP_OK) {
            strncpy(g_backend_port, "8080", sizeof(g_backend_port) - 1);
        }
    }

    if (!s_nvs_wifi_pass_key_present || g_wifi_ssid[0] == '\0' || g_backend_ip[0] == '\0') {
        nvs_handle_t legacy;
        if (nvs_open("wifi_config", NVS_READONLY, &legacy) == ESP_OK) {
            if (g_wifi_ssid[0] == '\0') {
                sz = sizeof(g_wifi_ssid);
                (void)nvs_get_str(legacy, "ssid", g_wifi_ssid, &sz);
            }
            if (!s_nvs_wifi_pass_key_present) {
                sz = sizeof(g_wifi_pass);
                if (nvs_get_str(legacy, "pass", g_wifi_pass, &sz) == ESP_OK) {
                    s_nvs_wifi_pass_key_present = true;
                } else {
                    sz = sizeof(g_wifi_pass);
                    if (nvs_get_str(legacy, NVS_KEY_WIFI_PASS, g_wifi_pass, &sz) == ESP_OK) {
                        s_nvs_wifi_pass_key_present = true;
                    }
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

    (void)nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid);
    (void)nvs_set_str(handle, "ssid", ssid);

    // A non-NULL pass (including "") is an explicit value; NULL preserves the
    // currently stored password. This lets provisioning switch to an open AP.
    if (pass != NULL) {
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

static esp_err_t wifi_set_sta_config(const char *ssid, const char *pass) {
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = false;
    cfg.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

bool wifi_apply_runtime_credentials(const char *ssid, const char *pass,
                                    const char *backend_ip, const char *backend_port) {
    if (!ssid || ssid[0] == '\0') return false;

    char new_ssid[sizeof(g_wifi_ssid)] = {0};
    str_trim_copy(new_ssid, sizeof(new_ssid), ssid);
    if (new_ssid[0] == '\0') return false;

    char new_pass[sizeof(g_wifi_pass)] = {0};
    if (pass != NULL) {
        size_t pass_len = strnlen(pass, sizeof(new_pass));
        if (pass_len >= sizeof(new_pass)) {
            ESP_LOGE(TAG, "Rejected: password too long (%u)", (unsigned)pass_len);
            return false;
        }
        if (pass_len > 0 && (pass_len < 8 || pass_len > 63)) {
            ESP_LOGE(TAG, "Rejected: WPA password must be 8-63 characters");
            return false;
        }
        str_trim_copy(new_pass, sizeof(new_pass), pass);
    }

    char new_ip[sizeof(g_backend_ip)] = {0};
    if (backend_ip && backend_ip[0] != '\0') {
        if (!is_valid_lan_ip(backend_ip)) {
            ESP_LOGE(TAG, "REJECTED: Backend IP %s is a Docker/loopback or non-LAN address", backend_ip);
            return false;
        }
        str_trim_copy(new_ip, sizeof(new_ip), backend_ip);
    }

    char new_port[sizeof(g_backend_port)] = {0};
    if (backend_port && backend_port[0] != '\0') {
        int p = atoi(backend_port);
        if (p <= 0 || p > 65535) {
            ESP_LOGE(TAG, "Rejected: invalid backend port %s", backend_port);
            return false;
        }
        str_trim_copy(new_port, sizeof(new_port), backend_port);
    }

    char old_ssid[sizeof(g_wifi_ssid)];
    char old_pass[sizeof(g_wifi_pass)];
    char old_ip[sizeof(g_backend_ip)];
    char old_port[sizeof(g_backend_port)];
    str_trim_copy(old_ssid, sizeof(old_ssid), g_wifi_ssid);
    str_trim_copy(old_pass, sizeof(old_pass), g_wifi_pass);
    str_trim_copy(old_ip, sizeof(old_ip), g_backend_ip);
    str_trim_copy(old_port, sizeof(old_port), g_backend_port);

    str_trim_copy(g_wifi_ssid, sizeof(g_wifi_ssid), new_ssid);
    if (pass != NULL) str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), new_pass);
    if (new_ip[0] != '\0') str_trim_copy(g_backend_ip, sizeof(g_backend_ip), new_ip);
    if (new_port[0] != '\0') str_trim_copy(g_backend_port, sizeof(g_backend_port), new_port);
    s_discovered_ip[0] = '\0';

    char target_url[URL_BUFFER_SIZE];
    if (get_backend_url(CAMERA_FRAME_ENDPOINT, target_url, sizeof(target_url)) == ESP_OK) {
        ESP_LOGI(TAG, "Target backend URL: %s", target_url);
    }

    if (!s_wifi_initialized) {
        return wifi_nvs_save_full_config(g_wifi_ssid, pass != NULL ? g_wifi_pass : NULL,
                                         g_backend_ip, g_backend_port);
    }

    s_provisioning_active = true;
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
    if (g_wifi_event_group) {
        xEventGroupClearBits(g_wifi_event_group,
                             WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    (void)esp_wifi_disconnect();
    // Let the STA leave an in-flight connect/retry before changing credentials.
    // Otherwise esp_wifi_connect() returns ESP_ERR_WIFI_STATE and the new AP is
    // never attempted until the old retry cycle finishes. The disconnect event
    // signals WIFI_FAIL_BIT; fall back to a bounded delay for the already-idle
    // case where no event is emitted.
    if (g_wifi_event_group) {
        (void)xEventGroupWaitBits(g_wifi_event_group, WIFI_FAIL_BIT, pdTRUE,
                                  pdFALSE, pdMS_TO_TICKS(1200));
    }
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_err_t set_err = ESP_ERR_WIFI_STATE;
    for (int attempt = 0; attempt < 10; ++attempt) {
        set_err = wifi_set_sta_config(g_wifi_ssid, g_wifi_pass);
        if (set_err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply new Wi-Fi config: %s",
                 esp_err_to_name(set_err));
        str_trim_copy(g_wifi_ssid, sizeof(g_wifi_ssid), old_ssid);
        str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), old_pass);
        str_trim_copy(g_backend_ip, sizeof(g_backend_ip), old_ip);
        str_trim_copy(g_backend_port, sizeof(g_backend_port), old_port);
        s_provisioning_active = false;
        if (s_reconnect_timer != NULL && old_ssid[0] != '\0') {
            (void)esp_timer_start_once(s_reconnect_timer, 3000000ULL);
        }
        return false;
    }

    esp_err_t connect_err = esp_wifi_connect();
    for (int attempt = 0; attempt < 10 && connect_err == ESP_ERR_WIFI_STATE;
         ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(200));
        connect_err = esp_wifi_connect();
    }
    if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "esp_wifi_connect returned %s",
                 esp_err_to_name(connect_err));
    }

    bool connected = false;
    if (g_wifi_event_group) {
        EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE, pdMS_TO_TICKS(25000));
        connected = (bits & WIFI_CONNECTED_BIT) != 0;
    }

    if (!connected) {
        ESP_LOGE(TAG, "Provisioning failed to connect; restoring previous credentials");
        str_trim_copy(g_wifi_ssid, sizeof(g_wifi_ssid), old_ssid);
        str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), old_pass);
        str_trim_copy(g_backend_ip, sizeof(g_backend_ip), old_ip);
        str_trim_copy(g_backend_port, sizeof(g_backend_port), old_port);
        if (g_wifi_event_group) {
            xEventGroupClearBits(g_wifi_event_group,
                                 WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(120));
        if (old_ssid[0] != '\0') {
            (void)wifi_set_sta_config(g_wifi_ssid, g_wifi_pass);
        }
        s_provisioning_active = false;
        // Leave the STA idle briefly so a retry from the UI can apply new
        // credentials instead of racing a background reconnect to the old AP.
        if (s_reconnect_timer != NULL && old_ssid[0] != '\0') {
            (void)esp_timer_start_once(s_reconnect_timer, 3000000ULL);
        }
        return false;
    }

    s_provisioning_active = false;
    if (!wifi_nvs_save_full_config(g_wifi_ssid, pass != NULL ? g_wifi_pass : NULL,
                                   g_backend_ip, g_backend_port)) {
        ESP_LOGW(TAG, "Connected but failed to persist credentials to NVS");
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

    // UDP discovery reports the source address that actually reached the backend.
    // Prefer it over an old NVS/Kconfig address after DHCP or network changes.
    if (is_same_subnet_ip(s_discovered_ip)) {
        strncpy(out_ip, s_discovered_ip, max_len - 1);
        out_ip[max_len - 1] = '\0';
        return true;
    }

    esp_netif_ip_info_t ip_info;
    if (get_sta_ip_info(&ip_info)) {
        if (is_valid_ipv4(g_backend_ip) && is_same_subnet_ip(g_backend_ip)) {
            strncpy(out_ip, g_backend_ip, max_len - 1);
            out_ip[max_len - 1] = '\0';
            return true;
        }
        if (is_valid_ipv4(CONFIG_BACKEND_IP) && is_same_subnet_ip(CONFIG_BACKEND_IP)) {
            strncpy(out_ip, CONFIG_BACKEND_IP, max_len - 1);
            out_ip[max_len - 1] = '\0';
            return true;
        }
        if (ip_info.gw.addr != 0) {
            snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.gw));
            return true;
        }
    }

    // Preserve explicitly configured non-local backends when STA has no lease.
    if (!get_sta_ip_info(&ip_info) && is_valid_ipv4(g_backend_ip)) {
        strncpy(out_ip, g_backend_ip, max_len - 1);
        out_ip[max_len - 1] = '\0';
        return true;
    }
    if (!get_sta_ip_info(&ip_info) && is_valid_ipv4(CONFIG_BACKEND_IP)) {
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
    if (g_wifi_event_group) {
        xEventGroupSetBits(g_wifi_event_group, WIFI_DISCOVERY_BIT);
    }
}

bool wifi_camera_stream_is_healthy(void) {
    return is_camera_streaming();
}

static void wifi_reconnect_timer_cb(void *arg) {
    (void)arg;
    if (s_provisioning_active) return;
    if (g_wifi_ssid[0] != '\0') {
        (void)esp_wifi_connect();
    }
}

static bool wifi_discover_backend(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Discovery: socket() failed (%d)", errno);
        return false;
    }

    int broadcast = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(BACKEND_DISCOVERY_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const char *probe = BACKEND_DISCOVERY_MAGIC;
    if (sendto(sock, probe, strlen(probe), 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        ESP_LOGW(TAG, "Discovery: broadcast failed (%d)", errno);
        close(sock);
        return false;
    }

    char buf[256];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &src_len);
    if (n <= 0) {
        close(sock);
        return false;
    }
    buf[n] = '\0';

    char ip[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip))) {
        close(sock);
        return false;
    }
    if (!is_same_subnet_ip(ip)) {
        ESP_LOGW(TAG, "Discovery: ignoring reply from %s (not on ESP subnet)", ip);
        close(sock);
        return false;
    }
    if (ntohs(src.sin_port) != BACKEND_DISCOVERY_PORT) {
        ESP_LOGW(TAG, "Discovery: ignoring reply from %s:%u (unexpected source port)",
                 ip, (unsigned)ntohs(src.sin_port));
        close(sock);
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "Discovery: ignoring malformed reply from %s", ip);
        close(sock);
        return false;
    }
    cJSON *service = cJSON_GetObjectItemCaseSensitive(root, "service");
    if (!cJSON_IsString(service) || strcmp(service->valuestring, "bawal-vision") != 0) {
        ESP_LOGW(TAG, "Discovery: ignoring reply from %s (unexpected service)", ip);
        cJSON_Delete(root);
        close(sock);
        return false;
    }

    char port_buf[sizeof(g_backend_port)] = {0};
    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "backend_port");
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535) {
        snprintf(port_buf, sizeof(port_buf), "%d", port->valueint);
    }
    cJSON_Delete(root);

    bool changed = strcmp(g_backend_ip, ip) != 0;
    str_trim_copy(g_backend_ip, sizeof(g_backend_ip), ip);
    str_trim_copy(s_discovered_ip, sizeof(s_discovered_ip), ip);
    if (port_buf[0] != '\0') {
        if (strcmp(g_backend_port, port_buf) != 0) changed = true;
        str_trim_copy(g_backend_port, sizeof(g_backend_port), port_buf);
    }

    if (changed) {
        ESP_LOGI(TAG, "Discovery: backend found at %s:%s", g_backend_ip, g_backend_port);
        (void)wifi_nvs_save_full_config(g_wifi_ssid, NULL, g_backend_ip, g_backend_port);
    }
    close(sock);
    return true;
}

static void wifi_discovery_task(void *pvParameters) {
    (void)pvParameters;
    uint32_t backoff_ms = DISCOVERY_BACKOFF_MIN_MS;

    for (;;) {
        // Wake on a failure notification, but also refresh periodically so a
        // restarted backend is re-adopted even while telemetry keeps succeeding.
        xEventGroupWaitBits(g_wifi_event_group, WIFI_DISCOVERY_BIT, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(DISCOVERY_REFRESH_MS));

        if (!(xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            continue;
        }

        if (wifi_discover_backend()) {
            backoff_ms = DISCOVERY_BACKOFF_MIN_MS;
            continue;
        }

        ESP_LOGW(TAG, "Discovery failed, retrying in %u ms", (unsigned)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));

        backoff_ms *= 2;
        if (backoff_ms > DISCOVERY_BACKOFF_MAX_MS) {
            backoff_ms = DISCOVERY_BACKOFF_MAX_MS;
        }
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
            xEventGroupSetBits(g_wifi_event_group, WIFI_FAIL_BIT);
        }
        lcd_update_wifi_status(false, NULL);
        lcd_update_http_status("WIFI LOST", true);
        if (s_provisioning_active) {
            // Provisioning owns the STA state machine; a background retry here
            // would race the new credentials and produce a spurious 'error'.
            return;
        }
        g_wifi_retry_num++;
        uint32_t delay_ms = (g_wifi_retry_num < 5) ? 1000 : 3000;
        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
        } else {
            ESP_LOGW(TAG, "Reconnect timer unavailable; retrying immediately");
            (void)esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[24] = {0};
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Obtained IP: %s gw: " IPSTR " mask: " IPSTR, ip_str,
                 IP2STR(&event->ip_info.gw), IP2STR(&event->ip_info.netmask));
        g_wifi_retry_num = 0;
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        if (g_wifi_event_group) {
            xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCOVERY_BIT);
            xEventGroupClearBits(g_wifi_event_group, WIFI_FAIL_BIT);
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
    if (g_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return;
    }
    if (xTaskCreate(wifi_discovery_task, "wifi_discovery", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start backend discovery task");
    }
    const esp_timer_create_args_t timer_args = {
        .callback = wifi_reconnect_timer_cb,
        .name = "wifi_reconnect_timer",
    };
    if (esp_timer_create(&timer_args, &s_reconnect_timer) != ESP_OK) {
        s_reconnect_timer = NULL;
        ESP_LOGE(TAG, "Failed to create Wi-Fi reconnect timer");
    }

    if (!wifi_nvs_load_credentials()) {
        str_trim_copy(g_wifi_ssid, sizeof(g_wifi_ssid), CONFIG_ESP_WIFI_SSID);
        str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), CONFIG_ESP_WIFI_PASSWORD);
        str_trim_copy(g_backend_ip, sizeof(g_backend_ip), CONFIG_BACKEND_IP);
        str_trim_copy(g_backend_port, sizeof(g_backend_port), CONFIG_BACKEND_PORT);
    } else {
        // NVS may have a valid SSID but no password key yet. Fall back per
        // field instead of treating that partial NVS record as complete.
        if (!s_nvs_wifi_pass_key_present) {
            str_trim_copy(g_wifi_pass, sizeof(g_wifi_pass), CONFIG_ESP_WIFI_PASSWORD);
        }
        if (g_backend_ip[0] == '\0') {
            str_trim_copy(g_backend_ip, sizeof(g_backend_ip), CONFIG_BACKEND_IP);
        }
        if (g_backend_port[0] == '\0') {
            str_trim_copy(g_backend_port, sizeof(g_backend_port), CONFIG_BACKEND_PORT);
        }
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
    wifi_cfg.sta.threshold.authmode = (g_wifi_pass[0] != '\0') ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = false;
    wifi_cfg.sta.pmf_cfg.required = false;
    ESP_LOGI(TAG, "STA security config: password_present=%s auth_threshold=%d PMF=off",
             g_wifi_pass[0] != '\0' ? "yes" : "no",
             (int)wifi_cfg.sta.threshold.authmode);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (g_wifi_ssid[0] != '\0') {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // Keep the Wi-Fi radio always on (no power save)
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
        err = httpd_resp_send(req, (const char *)frame.data, frame.header.buffer_len);
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Camera frame not ready");
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
        int64_t last_send_us = esp_timer_get_time();

        for (;;) {
            const int64_t cycle_start_us = esp_timer_get_time();
            esp_err_t copied = camera_copy_current_frame(&frame);
            if (copied != ESP_OK || frame.header.buffer_len == 0 ||
                frame.header.frame_id == last_frame_id) {
                if (esp_timer_get_time() - last_send_us > MJPEG_IDLE_TIMEOUT_US) {
                    ESP_LOGW(TAG, "Closing idle MJPEG client (no frame for 10 s)");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
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
            last_send_us = esp_timer_get_time();

            // Limit stream work to the 15 FPS budget while yielding immediately
            // when copying or sending already consumed the full cycle.
            const int64_t elapsed_us = esp_timer_get_time() - cycle_start_us;
            const int64_t remaining_us = MJPEG_TARGET_CYCLE_US - elapsed_us;
            if (remaining_us > 0) {
                const TickType_t delay_ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
                if (delay_ticks > 0) {
                    vTaskDelay(delay_ticks);
                } else {
                    taskYIELD();
                }
            } else {
                taskYIELD();
            }
        }
        free(frame.data);
    }

    (void)httpd_resp_send_chunk(req, NULL, 0);
    (void)httpd_req_async_handler_complete(req);
    atomic_fetch_sub(&s_mjpeg_clients, 1);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req) {
    int active = atomic_load(&s_mjpeg_clients);
    if (active >= MJPEG_CLIENT_LIMIT) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Maximum MJPEG clients reached");
    }

    // A stalled browser/backend must not pin this stream task while the camera
    // keeps capturing. Bound each TCP send to 400 ms so the client reconnects.
    const struct timeval send_timeout = {.tv_sec = 0, .tv_usec = 400000};
    const int client_fd = httpd_req_to_sockfd(req);
    if (client_fd >= 0) {
        (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                         &send_timeout, sizeof(send_timeout));
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
    config.max_open_sockets = 6;
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

    ESP_LOGI(TAG, "Native MJPEG server ready at http://<esp32-ip>/stream (VGA)");
    return ESP_OK;
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
    cJSON_AddNumberToObject(root, "free_heap_internal",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_heap_psram",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(json);

    if (err == ESP_OK && (http_status < 200 || http_status >= 300)) {
        ESP_LOGW(TAG, "Telemetry rejected with HTTP %d", http_status);
        return ESP_FAIL;
    }
    return err;
}

void WiFi_Server_Task(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!(g_wifi_event_group && (xEventGroupGetBits(g_wifi_event_group) & WIFI_CONNECTED_BIT))) continue;

        char backend_ip[32];
        if (get_backend_ip(backend_ip, sizeof(backend_ip))) {
            esp_err_t err = send_telemetry_to_backend(backend_ip, get_backend_port(), NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Telemetry to %s:%u failed: %s; refreshing backend discovery",
                         backend_ip, get_backend_port(), esp_err_to_name(err));
                wifi_notify_backend_failure();
            }
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
            // Omitted password keeps the stored one; an explicit "" targets an open AP.
            const char *pass_val = (cJSON_IsString(p) && p->valuestring) ? p->valuestring : NULL;
            const char *ip_val = (cJSON_IsString(ip) && ip->valuestring) ? ip->valuestring : "";

            char port_buf[16] = "8080";
            if (cJSON_IsNumber(port)) {
                snprintf(port_buf, sizeof(port_buf), "%d", port->valueint);
            } else if (cJSON_IsString(port) && port->valuestring && port->valuestring[0] != '\0') {
                strncpy(port_buf, port->valuestring, sizeof(port_buf) - 1);
            }

            bool ok = wifi_apply_runtime_credentials(ssid_val, pass_val, ip_val, port_buf);
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
