#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_NAMESPACE "bawal_cfg"
#define NVS_KEY_WIFI_SSID    "wifi_ssid"
#define NVS_KEY_WIFI_PASS    "wifi_pass"
#define NVS_KEY_BACKEND_IP   "backend_ip"
#define NVS_KEY_BACKEND_PORT "backend_port"
#define BACKEND_DISCOVERY_PORT 8765
#define BACKEND_DISCOVERY_MAGIC "BAWAL_DISCOVER_V1"

#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID ""
#endif

#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD ""
#endif

#ifndef CONFIG_BACKEND_IP
#define CONFIG_BACKEND_IP ""
#endif

#ifndef CONFIG_BACKEND_PORT
#define CONFIG_BACKEND_PORT "8080"
#endif

#ifndef CONFIG_DEVICE_AUTH_TOKEN
#define CONFIG_DEVICE_AUTH_TOKEN "bawal_secure_token_prod"
#endif


#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_DISCOVERY_BIT BIT2

extern EventGroupHandle_t g_wifi_event_group;
extern char g_wifi_ssid[32];
extern char g_wifi_pass[64];
extern char g_backend_ip[32];
extern char g_backend_port[8];

/**
 * @brief Initialize Wi-Fi station mode and connect using NVS or build-time credentials.
 */
void wifi_init_sta(void);

/**
 * @brief Notify the legacy frame sender (retained for capture task compatibility).
 */
void wifi_notify_stream_frame_ready(void);

/**
 * @brief Load Wi-Fi and backend configuration from NVS.
 */
bool wifi_nvs_load_credentials(void);

/**
 * @brief Persist Wi-Fi credentials to NVS.
 */
bool wifi_nvs_save_credentials(const char *ssid, const char *pass);

/**
 * @brief Persist Wi-Fi credentials and backend address to NVS.
 */
bool wifi_nvs_save_full_config(const char *ssid, const char *pass, const char *backend_ip,
                               const char *backend_port);

/**
 * @brief Apply new Wi-Fi credentials at runtime, persist them, and reconnect.
 */
bool wifi_apply_runtime_credentials(const char *ssid, const char *pass, const char *backend_ip,
                                    const char *backend_port);

/**
 * @brief Listen on USB Serial for Wi-Fi provisioning JSON commands.
 */
void usb_serial_provisioning_task(void *pvParameters);

/**
 * @brief Return the effective backend TCP port.
 */
uint16_t get_backend_port(void);

/**
 * @brief Start the native ESP-IDF MJPEG server on port 80.
 */
esp_err_t wifi_http_stream_server_start(void);

#ifdef __cplusplus
}
#endif
