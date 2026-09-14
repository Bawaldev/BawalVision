#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "data.h"

#define NVS_NAMESPACE       "wifi_config"
#define MAXIMUM_RETRY       5
#define BACKEND_ENDPOINT    "/api/v1/telemetry"

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

#define URL_BUFFER_SIZE     128
#define JSON_BUFFER_SIZE    512

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

extern EventGroupHandle_t g_wifi_event_group;
extern int g_wifi_retry_num;
extern char g_wifi_ssid[32];
extern char g_wifi_pass[64];

void wifi_init_sta(void);
bool wifi_nvs_load_credentials(void);
bool wifi_nvs_save_credentials(const char *ssid, const char *pass);
esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port, const telemetry_data_t *data);
bool get_backend_url(char *out_url, size_t max_len);
void WiFi_Server_Task(void *pvParameters);