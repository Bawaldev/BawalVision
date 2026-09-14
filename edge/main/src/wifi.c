#include <string.h>
#include <stdio.h>
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
#include "cJSON.h"

#include "data.h"
#include "wifi.h"

EventGroupHandle_t g_wifi_event_group = NULL;
int g_wifi_retry_num = 0;

static const char *TAG = "TELEMETRY_DISPATCH";
static const char *TAG_WIFI = "WIFI_STA";
char g_wifi_ssid[32] = {0};
char g_wifi_pass[64] = {0};

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG_WIFI, "Connecting to AP...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* dis = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG_WIFI, "Connection lost (Reason code: %d)", dis->reason);

        if (g_wifi_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            g_wifi_retry_num++;
            ESP_LOGW(TAG_WIFI, "Retrying connection (%d/%d)...", g_wifi_retry_num, MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(g_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG_WIFI, "Failed to connect to AP after %d attempts.", MAXIMUM_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "Obtained IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG_WIFI, "Gateway IP: " IPSTR, IP2STR(&event->ip_info.gw));
        g_wifi_retry_num = 0;
        xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool get_backend_ip(char *out_ip, size_t max_len) {
    if (out_ip == NULL || max_len == 0) return false;

    // 1. Prioritaskan jika CONFIG_BACKEND_IP diisi di build args/.env
    if (strlen(CONFIG_BACKEND_IP) > 0) {
        snprintf(out_ip, max_len, "%s", CONFIG_BACKEND_IP);
        return true;
    }

    // 2. Fallback: Ambil Gateway IP dari interface netif
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

    if (!wifi_nvs_load_credentials()) {
        ESP_LOGW(TAG_WIFI, "NVS kosong/invalid. Menggunakan fallback dari build args.");
        strncpy(g_wifi_ssid, CONFIG_ESP_WIFI_SSID, sizeof(g_wifi_ssid) - 1);
        strncpy(g_wifi_pass, CONFIG_ESP_WIFI_PASSWORD, sizeof(g_wifi_pass) - 1);
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

    wifi_config.sta.threshold.authmode = (strlen(g_wifi_pass) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_LOGI(TAG_WIFI, "Attempting connection -> SSID: '%s' | PASS LEN: %d", 
             g_wifi_ssid, (int)strlen(g_wifi_pass));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        g_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("WIFI", "Terhubung ke AP SSID: %s", g_wifi_ssid);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE("WIFI", "Gagal terhubung ke SSID: %s", g_wifi_ssid);
    } else {
        ESP_LOGE("WIFI", "Timeout saat mencoba terhubung ke SSID: %s", g_wifi_ssid);
    }
}

bool wifi_nvs_load_credentials(void) {
    nvs_handle_t nvs_handle;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Gagal membuka NVS (err: %s)", esp_err_to_name(err));
        return false;
    }

    size_t ssid_len = sizeof(g_wifi_ssid);
    size_t pass_len = sizeof(g_wifi_pass);

    esp_err_t err_ssid = nvs_get_str(nvs_handle, "ssid", g_wifi_ssid, &ssid_len);
    esp_err_t err_pass = nvs_get_str(nvs_handle, "pass", g_wifi_pass, &pass_len);

    nvs_close(nvs_handle);

    if (err_ssid == ESP_OK && err_pass == ESP_OK && strlen(g_wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Berhasil membaca kredensial dari NVS (SSID: %s)", g_wifi_ssid);
        return true;
    }

    ESP_LOGW(TAG, "Kredensial tidak ditemukan atau kosong di NVS");
    return false;
}

bool wifi_nvs_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal membuka NVS untuk menulis: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs_handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "pass", pass);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Berhasil menyimpan WiFi ke NVS: %s", ssid);
    } else {
        ESP_LOGE(TAG, "Gagal menyimpan ke NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return (err == ESP_OK);
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

    // 1. Prioritaskan BACKEND_IP yang dikirim dari .env / build args
    if (strlen(CONFIG_BACKEND_IP) > 0) {
        int ret = snprintf(out_url, max_len, "http://%s:%s%s", 
                           CONFIG_BACKEND_IP, CONFIG_BACKEND_PORT, BACKEND_ENDPOINT);
        return (ret > 0 && ret < (int)max_len);
    }

    // 2. Fallback: Gunakan Gateway IP jika BACKEND_IP kosong
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

    // Ambil pemakaian CPU real-time jika belum diisi oleh producer
    if (data->current_cpu_load == 0) {
        data->current_cpu_load = get_cpu_usage_percentage();
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "device_id", data->device_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)data->timestamp);
    cJSON_AddNumberToObject(root, "temperature", data->temperature);
    cJSON_AddNumberToObject(root, "humidity", data->humidity);
    cJSON_AddNumberToObject(root, "gas_voc", data->gas_voc);
    cJSON_AddNumberToObject(root, "pressure", data->pressure);
    cJSON_AddNumberToObject(root, "cpu_load", data->current_cpu_load); // Konsisten dengan backend Go & Postgres

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

    ESP_LOGI(TAG, "Dispatching telemetry to: %s (CPU Load: %u%%)", url, data->current_cpu_load);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Ingest completed. Status = %d", status_code);
    } else {
        ESP_LOGE(TAG, "Ingest failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    cJSON_free(json_string);

    return err;
}

void WiFi_Server_Task(void *pvParameters) {
    QueueHandle_t data_queue = (QueueHandle_t)pvParameters;
    telemetry_data_t telemetry;

    for (;;) {
        if (xQueueReceive(data_queue, &telemetry, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        char backend_ip[32] = {0};
        uint16_t backend_port = (uint16_t)atoi(CONFIG_BACKEND_PORT);

        // Gunakan get_backend_ip (mengembalikan string IP murni seperti "10.79.167.155")
        if (!get_backend_ip(backend_ip, sizeof(backend_ip))) {
            ESP_LOGE(TAG, "Failed to resolve backend gateway IP. Skipping dispatch.");
            continue;
        }

        send_telemetry_to_backend(backend_ip, backend_port, &telemetry);
    }
}