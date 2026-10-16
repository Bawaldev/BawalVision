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

#define NVS_NAMESPACE "wifi_config"
#define MAXIMUM_RETRY 5
#define BACKEND_ENDPOINT "/api/v1/telemetry"
#define CAMERA_FRAME_ENDPOINT "/api/v1/camera/frame"


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

#define URL_BUFFER_SIZE 128
#define JSON_BUFFER_SIZE 512

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

extern EventGroupHandle_t g_wifi_event_group;
extern int g_wifi_retry_num;
extern char g_wifi_ssid[32];
extern char g_wifi_pass[64];
extern char g_backend_ip[32];

/**
 * @brief Initialize Wi-Fi in station (STA) mode and register event handlers.
 *
 * Checks NVS for previously saved credentials, falls back to sdkconfig
 * defaults, and initiates connection attempts with exponential backoff retry.
 */
void wifi_init_sta(void);

/**
 * @brief Read saved Wi-Fi SSID and password from NVS storage.
 *
 * @return true if valid credentials were successfully retrieved, false
 * otherwise.
 */
bool wifi_nvs_load_credentials(void);

/**
 * @brief Save new Wi-Fi credentials into non-volatile storage (NVS).
 *
 * @param[in] ssid Wi-Fi network name (null-terminated).
 * @param[in] pass Wi-Fi network password (null-terminated).
 * @return true on successful commit to flash, false if writing failed.
 */
bool wifi_nvs_save_credentials(const char *ssid, const char *pass);

/**
 * @brief Save Wi-Fi credentials and target backend server IP into NVS.
 *
 * @param[in] ssid       Wi-Fi network name (null-terminated).
 * @param[in] pass       Wi-Fi network password (null-terminated).
 * @param[in] backend_ip Target backend IPv4 address (null-terminated).
 * @return true on successful commit to flash, false if writing failed.
 */
bool wifi_nvs_save_full_config(const char *ssid, const char *pass, const char *backend_ip);

/**
 * @brief Apply new Wi-Fi credentials at runtime, save to NVS, and reconnect STA.
 *
 * @param[in] ssid       Wi-Fi network name.
 * @param[in] pass       Wi-Fi network password.
 * @param[in] backend_ip Optional backend IPv4 address on the shared Wi-Fi network.
 * @return true if credentials were saved and applied to the Wi-Fi driver.
 */
bool wifi_apply_runtime_credentials(const char *ssid, const char *pass, const char *backend_ip);

/**
 * @brief FreeRTOS task listening on USB Serial/UART stdin for Wi-Fi provisioning commands from the server.
 *
 * Accepts JSON lines such as:
 * {"cmd":"provision_wifi","ssid":"ServerWiFi","pass":"secret","backend_ip":"192.168.1.10"}
 * or {"cmd":"status"} and responds with BAWAL_ACK:{...} over stdout.
 *
 * @param pvParameters Optional task parameters.
 */
void usb_serial_provisioning_task(void *pvParameters);

/**
 * @brief Serialize telemetry metrics to JSON and POST to backend REST server.
 *
 * @param[in] backend_ip IPv4 address or hostname string of backend server.
 * @param[in] port Destination HTTP port (e.g. 8080).
 * @param[in] data Pointer to telemetry data struct to transmit.
 * @return esp_err_t ESP_OK on successful HTTP 200/201 response, or error code.
 */
esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port,
                                    telemetry_data_t *data);

/**
 * @brief Encodes the latest camera frame and transmits it to the backend camera ingress endpoint.
 *
 * @param[in] backend_ip IPv4 address or hostname string of backend server.
 * @param[in] port Destination HTTP port (e.g. 8080).
 * @param[in] device_id Hardware device identifier.
 * @return esp_err_t ESP_OK on success, or error code.
 */
esp_err_t send_camera_frame_to_backend(const char *backend_ip, uint16_t port,
                                       const char *device_id);

/**
 * @brief Format complete telemetry endpoint URL string.
 *
 * @param[out] out_url Buffer to receive formatted URL (e.g.
 * "http://192.168.1.50:8080/api/v1/telemetry").
 * @param[in]  max_len Size of destination buffer in bytes.
 * @return true if URL was formatted without truncation, false otherwise.
 */
bool get_backend_url(char *out_url, size_t max_len);

/**
 * @brief Retrieve current target backend server IP address.
 *
 * @param[out] out_ip Destination buffer for IP string.
 * @param[in]  max_len Capacity of destination buffer in bytes.
 * @return true if IP was successfully copied, false otherwise.
 */
bool get_backend_ip(char *out_ip, size_t max_len);

/**
 * @brief FreeRTOS worker task for processing telemetry queues and sending data
 * over HTTP.
 *
 * Blocks on the incoming telemetry queue, manages Wi-Fi reconnection state,
 * and handles dispatching JSON payloads.
 *
 * @param pvParameters FreeRTOS queue handle (QueueHandle_t) to receive
 * telemetry from.
 */
void WiFi_Server_Task(void *pvParameters);

/**
 * @brief FreeRTOS task to continuously stream JPEG frames to the backend REST API via HTTP POST.
 * 
 * @param pvParameters Optional task parameters.
 */
void http_stream_sender_task(void *pvParameters);

#ifdef __cplusplus
}
#endif