#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "system_monitor.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "cJSON.h"

#include "data.h"
#include "wifi.h"
#include "lcd_task.h"
#include "camera_task.h"

EventGroupHandle_t g_wifi_event_group = NULL;
int g_wifi_retry_num = 0;

static const char *TAG = "TELEMETRY_DISPATCH";
static const char *TAG_WIFI = "WIFI_STA";
char g_wifi_ssid[32] = {0};
char g_wifi_pass[64] = {0};
char g_backend_ip[32] = {0};

static esp_http_client_handle_t s_stream_client = NULL;
static char s_cached_stream_url[URL_BUFFER_SIZE] = {0};

/**
 * @brief Cleanly teardown and reset camera HTTP streaming client.
 *
 * Ensures all hanging socket descriptors are completely closed and recycled
 * whenever Wi-Fi state shifts (association, disconnect, IP reassignment).
 */
static void wifi_reset_stream_client(void) {
    if (s_stream_client != NULL) {
        esp_http_client_close(s_stream_client);
        esp_http_client_cleanup(s_stream_client);
        s_stream_client = NULL;
        memset(s_cached_stream_url, 0, sizeof(s_cached_stream_url));
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_reset_stream_client();
        if (strlen(g_wifi_ssid) > 0) {
            esp_wifi_connect();
            ESP_LOGI(TAG_WIFI, "Connecting to AP '%s'...", g_wifi_ssid);
            lcd_update_http_status("WIFI CONNECTING...", false);
        } else {
            ESP_LOGW(TAG_WIFI, "Wi-Fi SSID not configured yet. Waiting for USB Serial provisioning from server...");
            lcd_update_http_status("WAIT USB WIFI...", false);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        /* Strictly disable Wi-Fi Modem Sleep upon association to eliminate socket dropouts & latency spikes */
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG_WIFI, "Associated with AP '%s' (Modem sleep disabled). Requesting IP via DHCP...", g_wifi_ssid);
        lcd_update_http_status("DHCP REQUEST...", false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_reset_stream_client();
        wifi_event_sta_disconnected_t* dis = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG_WIFI, "Connection lost (Reason code: %d)", dis->reason);
        if (g_wifi_event_group != NULL) {
            xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(false, NULL);
        lcd_update_http_status("WIFI LOST", true);

        if (strlen(g_wifi_ssid) > 0) {
            g_wifi_retry_num++;
            ESP_LOGW(TAG_WIFI, "Reconnecting to AP '%s' (attempt %d)...", g_wifi_ssid, g_wifi_retry_num);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* Enforce power save disabled on IP acquisition & reset stream sockets to bind cleanly to new IP */
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_reset_stream_client();
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[24];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG_WIFI, "Obtained IP: %s (PS_NONE enforced)", ip_str);
        ESP_LOGI(TAG_WIFI, "Gateway IP: " IPSTR, IP2STR(&event->ip_info.gw));
        g_wifi_retry_num = 0;
        if (g_wifi_event_group != NULL) {
            xEventGroupClearBits(g_wifi_event_group, WIFI_FAIL_BIT);
            xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        lcd_update_wifi_status(true, ip_str);
        lcd_update_http_status("WIFI READY", false);
    }
}

bool get_backend_ip(char *out_ip, size_t max_len) {
    if (out_ip == NULL || max_len == 0) return false;

    if (strlen(g_backend_ip) > 0) {
        snprintf(out_ip, max_len, "%s", g_backend_ip);
        return true;
    }
    if (strlen(CONFIG_BACKEND_IP) > 0) {
        snprintf(out_ip, max_len, "%s", CONFIG_BACKEND_IP);
        return true;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        netif = esp_netif_get_default_netif();
    }

    if (netif == NULL) {
        ESP_LOGE(TAG, "Network interface not found");
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "IP interface unassigned");
        return false;
    }

    if (ip_info.gw.addr != 0) {
        snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.gw));
        return true;
    }

    return false;
}

void wifi_init_sta(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Prioritize NVS credentials provisioned via USB Serial so server Wi-Fi sync persists across reboots */
    if (!wifi_nvs_load_credentials()) {
        if (strlen(CONFIG_ESP_WIFI_SSID) > 0) {
            strncpy(g_wifi_ssid, CONFIG_ESP_WIFI_SSID, sizeof(g_wifi_ssid) - 1);
            strncpy(g_wifi_pass, CONFIG_ESP_WIFI_PASSWORD, sizeof(g_wifi_pass) - 1);
        } else {
            ESP_LOGW(TAG_WIFI, "NVS empty/invalid and no build args provided. Waiting for USB Serial Wi-Fi provisioning.");
        }
    }
    if (strlen(g_backend_ip) == 0 && strlen(CONFIG_BACKEND_IP) > 0) {
        strncpy(g_backend_ip, CONFIG_BACKEND_IP, sizeof(g_backend_ip) - 1);
    }

    g_wifi_event_group = xEventGroupCreate();
    configASSERT(g_wifi_event_group != NULL);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &instance_any_id
    ));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &instance_got_ip
    ));

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", g_wifi_ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", g_wifi_pass);
    
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    wifi_config.sta.threshold.authmode = (strlen(g_wifi_pass) > 0) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG_WIFI, "Attempting connection -> SSID: '%s' | PASS LEN: %d", 
             g_wifi_ssid, (int)strlen(g_wifi_pass));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

bool wifi_nvs_load_credentials(void) {
    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS (err: %s)", esp_err_to_name(err));
        return false;
    }

    size_t ssid_len = sizeof(g_wifi_ssid);
    size_t pass_len = sizeof(g_wifi_pass);
    size_t ip_len = sizeof(g_backend_ip);

    esp_err_t err_ssid = nvs_get_str(nvs_handle, "ssid", g_wifi_ssid, &ssid_len);
    esp_err_t err_pass = nvs_get_str(nvs_handle, "pass", g_wifi_pass, &pass_len);
    (void)nvs_get_str(nvs_handle, "backend_ip", g_backend_ip, &ip_len);

    nvs_close(nvs_handle);

    if (err_ssid == ESP_OK && err_pass == ESP_OK && strlen(g_wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Successfully read credentials from NVS (SSID: %s, BackendIP: %s)", g_wifi_ssid, g_backend_ip);
        return true;
    }

    ESP_LOGW(TAG, "Credentials not found or empty in NVS");
    return false;
}

bool wifi_nvs_save_full_config(const char *ssid, const char *pass, const char *backend_ip) {
    if (ssid == NULL) return false;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs_handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "pass", pass != NULL ? pass : "");
    }
    if (err == ESP_OK && backend_ip != NULL && strlen(backend_ip) > 0) {
        err = nvs_set_str(nvs_handle, "backend_ip", backend_ip);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Successfully saved WiFi to NVS: %s (Backend: %s)", ssid, backend_ip != NULL ? backend_ip : "");
    } else {
        ESP_LOGE(TAG, "Failed to save to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return (err == ESP_OK);
}

bool wifi_nvs_save_credentials(const char *ssid, const char *pass) {
    return wifi_nvs_save_full_config(ssid, pass, NULL);
}

static bool is_safe_printable_str(const char *s, size_t max_len) {
    if (s == NULL) return false;
    size_t len = strlen(s);
    if (len > max_len) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

static bool is_valid_ipv4_str(const char *ip) {
    if (ip == NULL) return false;
    size_t len = strlen(ip);
    if (len < 7 || len > 15) return false;
    int dots = 0;
    for (size_t i = 0; i < len; ++i) {
        if (ip[i] == '.') {
            dots++;
        } else if (ip[i] < '0' || ip[i] > '9') {
            return false;
        }
    }
    return (dots == 3);
}

bool wifi_apply_runtime_credentials(const char *ssid, const char *pass, const char *backend_ip) {
    if (ssid == NULL || strlen(ssid) == 0 || !is_safe_printable_str(ssid, sizeof(g_wifi_ssid) - 1)) {
        return false;
    }
    if (pass != NULL && !is_safe_printable_str(pass, sizeof(g_wifi_pass) - 1)) {
        return false;
    }

    memset(g_wifi_ssid, 0, sizeof(g_wifi_ssid));
    memset(g_wifi_pass, 0, sizeof(g_wifi_pass));
    strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
    if (pass != NULL) {
        strncpy(g_wifi_pass, pass, sizeof(g_wifi_pass) - 1);
    }
    if (backend_ip != NULL && is_valid_ipv4_str(backend_ip)) {
        memset(g_backend_ip, 0, sizeof(g_backend_ip));
        strncpy(g_backend_ip, backend_ip, sizeof(g_backend_ip) - 1);
    }

    wifi_nvs_save_full_config(g_wifi_ssid, g_wifi_pass, g_backend_ip);

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", g_wifi_ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", g_wifi_pass);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = (strlen(g_wifi_pass) > 0) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    g_wifi_retry_num = 0;
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
        lcd_update_http_status("USB WIFI SYNC...", false);
        esp_wifi_connect();
        return true;
    }
    return false;
}

bool get_backend_url(char *out_url, size_t max_len) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "Network interface WIFI_STA_DEF not found");
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "IP interface unassigned");
        return false;
    }
    if (strlen(g_backend_ip) > 0) {
        int ret = snprintf(out_url, max_len, "http://%s:%s%s", 
                           g_backend_ip, CONFIG_BACKEND_PORT, BACKEND_ENDPOINT);
        return (ret > 0 && ret < (int)max_len);
    }
    if (strlen(CONFIG_BACKEND_IP) > 0) {
        int ret = snprintf(out_url, max_len, "http://%s:%s%s", 
                           CONFIG_BACKEND_IP, CONFIG_BACKEND_PORT, BACKEND_ENDPOINT);
        return (ret > 0 && ret < (int)max_len);
    }
    if (ip_info.gw.addr != 0) {
        int ret = snprintf(out_url, max_len, "http://" IPSTR ":%s%s", 
                           IP2STR(&ip_info.gw), CONFIG_BACKEND_PORT, BACKEND_ENDPOINT);
        return (ret > 0 && ret < (int)max_len);
    }

    return false;
}

esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port,telemetry_data_t *data) {
    if (backend_ip == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[URL_BUFFER_SIZE];
    snprintf(url, sizeof(url), "http://%s:%d%s", backend_ip, port, BACKEND_ENDPOINT);

    if (data->current_cpu_load == 0) {
        data->current_cpu_load = get_cpu_usage_percentage();
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "device_id", data->device_id);
    cJSON_AddStringToObject(root, "firmware_version", data->firmware_version);
    cJSON_AddNumberToObject(root, "timestamp", (double)data->timestamp);
    float send_temp = data->temperature;
    if (send_temp <= 0.0f) {
        send_temp = get_esp32_cpu_temperature();
    }
    cJSON_AddNumberToObject(root, "temperature", send_temp);
    cJSON_AddNumberToObject(root, "cpu_load", data->current_cpu_load);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", (double)ap_info.rssi);
    }
    cJSON_AddStringToObject(root, "device_name", data->device_name);
    cJSON_AddNumberToObject(root, "device_core", data->device_core);
    cJSON_AddStringToObject(root, "camera_model", camera_get_sensor_name());

    char *json_string = cJSON_PrintUnformatted(root);
    if (json_string == NULL) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize esp_http_client");
        cJSON_Delete(root);
        cJSON_free(json_string);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_string, strlen(json_string));

    lcd_update_http_status("POST TELEMETRY...", false);
    lcd_update_telemetry_display(data->temperature, data->current_cpu_load);

    ESP_LOGI(TAG, "Dispatching telemetry to: %s (CPU Load: %u%%)", url, data->current_cpu_load);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Ingest completed. Status = %d", status_code);
        char status_buf[32];
        snprintf(status_buf, sizeof(status_buf), "HTTP %d (OK)", status_code);
        lcd_update_http_status(status_buf, false);
    } else {
        ESP_LOGE(TAG, "Ingest failed: %s", esp_err_to_name(err));
        lcd_update_http_status("HTTP: TIMEOUT", true);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    cJSON_free(json_string);

    return err;
}

esp_err_t send_camera_frame_to_backend(const char *backend_ip, uint16_t port, const char *device_id) {
    if (backend_ip == NULL || device_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static camera_frame_buffer_t s_local_tx_frame = {0};
    if (s_local_tx_frame.data == NULL) {
        s_local_tx_frame.capacity = INITIAL_BUF_CAPACITY;
        s_local_tx_frame.data = (uint8_t *)heap_caps_malloc(s_local_tx_frame.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_local_tx_frame.data == NULL) {
            s_local_tx_frame.data = (uint8_t *)malloc(s_local_tx_frame.capacity);
        }
    }
    if (s_local_tx_frame.data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (camera_copy_latest_frame(&s_local_tx_frame) != ESP_OK || s_local_tx_frame.header.buffer_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char url[URL_BUFFER_SIZE];
    snprintf(url, sizeof(url), "http://%s:%d%s", backend_ip, port, CAMERA_FRAME_ENDPOINT);

    /* Reuse persistent HTTP client with TCP Keep-Alive to eliminate per-frame socket handshakes */
    if (s_stream_client == NULL || strcmp(s_cached_stream_url, url) != 0) {
        wifi_reset_stream_client();
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 4000,
            .buffer_size_tx = 16384,
            .keep_alive_enable = true,
        };
        s_stream_client = esp_http_client_init(&config);
        if (s_stream_client == NULL) {
            return ESP_FAIL;
        }
        strncpy(s_cached_stream_url, url, sizeof(s_cached_stream_url) - 1);
    }

    char frame_id_str[16];
    snprintf(frame_id_str, sizeof(frame_id_str), "%lu", (unsigned long)s_local_tx_frame.header.frame_id);

    char temp_str[16];
    char load_str[16];
    float real_temp = get_esp32_cpu_temperature();
    uint8_t cpu_load = get_cpu_usage_percentage();
    snprintf(temp_str, sizeof(temp_str), "%.2f", real_temp);
    snprintf(load_str, sizeof(load_str), "%u", cpu_load);

    esp_http_client_set_header(s_stream_client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(s_stream_client, "Connection", "keep-alive");
    esp_http_client_set_header(s_stream_client, "X-Device-ID", device_id);
    esp_http_client_set_header(s_stream_client, "X-Frame-ID", frame_id_str);
    esp_http_client_set_header(s_stream_client, "X-ESP-Temp", temp_str);
    esp_http_client_set_header(s_stream_client, "X-ESP-CPU-Load", load_str);
    esp_http_client_set_header(s_stream_client, "X-Camera-Model", camera_get_sensor_name());
    esp_http_client_set_post_field(s_stream_client, (const char *)s_local_tx_frame.data, s_local_tx_frame.header.buffer_len);

    esp_err_t err = esp_http_client_perform(s_stream_client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(s_stream_client);
        if (status != 200 && status != 201) {
            ESP_LOGW(TAG, "Stream rejected with status %d, resetting client", status);
            wifi_reset_stream_client();
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "Stream socket error (%s), recycling connection", esp_err_to_name(err));
        wifi_reset_stream_client();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return err;
}

void http_stream_sender_task(void *pvParameters) {
    (void)pvParameters;
    char backend_ip[32] = {0};
    uint16_t backend_port = (uint16_t)atoi(CONFIG_BACKEND_PORT);
    const TickType_t frame_interval_ticks = pdMS_TO_TICKS(1000 / STREAM_FPS);

    ESP_LOGI(TAG, "Dedicated HTTP stream sender task running (Target: %d FPS)", STREAM_FPS);

    for (;;) {
        TickType_t start_tick = xTaskGetTickCount();

        /* Wait for Wi-Fi station connectivity before attempting HTTP transmission */
        if (g_wifi_event_group != NULL) {
            EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
            if (!(bits & WIFI_CONNECTED_BIT)) {
                wifi_reset_stream_client();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char dev_id[32] = {0};
        get_internal_device_id(dev_id, sizeof(dev_id));
        const char *target_device_id = (dev_id[0] != '\0') ? dev_id : DEVICE_ID;
        esp_err_t err = send_camera_frame_to_backend(backend_ip, backend_port, target_device_id);
        if (err == ESP_ERR_NOT_FOUND) {
            /* No new frame ready yet from camera ISP, yield briefly */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks < frame_interval_ticks) {
            vTaskDelay(frame_interval_ticks - elapsed_ticks);
        } else {
            taskYIELD();
        }
    }
}

void WiFi_Server_Task(void *pvParameters) {
    QueueHandle_t data_queue = (QueueHandle_t)pvParameters;
    telemetry_data_t telemetry;

    for (;;) {
        if (xQueueReceive(data_queue, &telemetry, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (g_wifi_event_group != NULL) {
            EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
            if (!(bits & WIFI_CONNECTED_BIT)) {
                static uint32_t s_skip_count = 0;
                if ((s_skip_count++ % 6) == 0) {
                    ESP_LOGW(TAG, "Wi-Fi not connected (SSID '%s', retries %d). Telemetry dropped (%lu so far).",
                             g_wifi_ssid, g_wifi_retry_num, (unsigned long)s_skip_count);
                }
                continue;
            }
        }

        char backend_ip[32] = {0};
        uint16_t backend_port = (uint16_t)atoi(CONFIG_BACKEND_PORT);
        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
            ESP_LOGE(TAG, "Failed to resolve backend gateway IP. Skipping dispatch.");
            continue;
        }

        /* If camera stream is actively transmitting, telemetry is already delivered
           on every single frame in HTTP headers (X-ESP-Temp, X-ESP-CPU-Load).
           Bypass standalone POST to completely eliminate socket contention on fd 54. */
        if (is_camera_streaming()) {
            continue;
        }

        send_telemetry_to_backend(backend_ip, backend_port, &telemetry);
    }
}

void usb_serial_provisioning_task(void *pvParameters) {
    (void)pvParameters;
    char line_buf[256];
    size_t idx = 0;

    telemetry_data_t meta;
    init_device_metadata(&meta);

    ESP_LOGI(TAG_WIFI, "USB Serial Wi-Fi Provisioning listener active on stdin (Device ID: %s)", meta.device_id);

    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (idx == 0) {
                continue;
            }
            line_buf[idx] = '\0';
            idx = 0;

            /* Locate JSON start in case of serial noise */
            char *json_start = strchr(line_buf, '{');
            if (json_start == NULL) {
                continue;
            }

            cJSON *cmd_json = cJSON_Parse(json_start);
            if (cmd_json == NULL) {
                continue;
            }

            cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "cmd");
            if (cJSON_IsString(cmd_item) && cmd_item->valuestring != NULL) {
                if (strcmp(cmd_item->valuestring, "provision_wifi") == 0 ||
                    strcmp(cmd_item->valuestring, "set_wifi") == 0) {
                    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "ssid");
                    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "pass");
                    cJSON *ip_item = cJSON_GetObjectItemCaseSensitive(cmd_json, "backend_ip");

                    const char *new_ssid = (cJSON_IsString(ssid_item) && ssid_item->valuestring) ? ssid_item->valuestring : "";
                    const char *new_pass = (cJSON_IsString(pass_item) && pass_item->valuestring) ? pass_item->valuestring : "";
                    const char *new_ip = (cJSON_IsString(ip_item) && ip_item->valuestring) ? ip_item->valuestring : "";

                    bool ok = wifi_apply_runtime_credentials(new_ssid, new_pass, new_ip);
                    float real_temp = get_esp32_cpu_temperature();
                    uint8_t cpu_load = get_cpu_usage_percentage();

                    printf("BAWAL_ACK:{\"status\":\"%s\",\"device_id\":\"%s\",\"firmware_version\":\"%s\",\"ssid\":\"%s\",\"backend_ip\":\"%s\",\"temperature\":%.2f,\"cpu_load\":%u}\n",
                           ok ? "wifi_provisioned" : "error",
                           meta.device_id,
                           meta.firmware_version,
                           g_wifi_ssid,
                           g_backend_ip,
                           real_temp,
                           cpu_load);
                    fflush(stdout);
                } else if (strcmp(cmd_item->valuestring, "status") == 0 ||
                           strcmp(cmd_item->valuestring, "get_status") == 0) {
                    bool wifi_connected = false;
                    if (g_wifi_event_group != NULL) {
                        EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);
                        wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;
                    }
                    float real_temp = get_esp32_cpu_temperature();
                    uint8_t cpu_load = get_cpu_usage_percentage();
                    printf("BAWAL_ACK:{\"status\":\"ok\",\"device_id\":\"%s\",\"firmware_version\":\"%s\",\"wifi_connected\":%s,\"ssid\":\"%s\",\"backend_ip\":\"%s\",\"temperature\":%.2f,\"cpu_load\":%u}\n",
                           meta.device_id,
                           meta.firmware_version,
                           wifi_connected ? "true" : "false",
                           g_wifi_ssid,
                           g_backend_ip,
                           real_temp,
                           cpu_load);
                    fflush(stdout);
                }
            }
            cJSON_Delete(cmd_json);
            memset(line_buf, 0, sizeof(line_buf));
        } else if (idx < sizeof(line_buf) - 1) {
            line_buf[idx++] = (char)ch;
        } else {
            memset(line_buf, 0, sizeof(line_buf));
            idx = 0;
        }
    }
}