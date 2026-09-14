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
        if (g_wifi_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            g_wifi_retry_num++;
            ESP_LOGW(TAG_WIFI, "Connection lost. Retrying connection (%d/%d)...", g_wifi_retry_num, MAXIMUM_RETRY);
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

void wifi_init_sta(void) {
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
    
    // Authmode threshold disesuaikan dengan isi password
    wifi_config.sta.threshold.authmode = (strlen(g_wifi_pass) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG_WIFI, "Connecting with SSID: '%s'", g_wifi_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Timeout 15 detik agar tidak menggantung selamanya jika AP tidak merespon
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
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.gw.addr == 0) {
        ESP_LOGW(TAG, "Gateway IP unavailable or interface unassigned");
        return false;
    }

    int ret = snprintf(out_url, max_len, "http://" IPSTR ":8080/api/v1/telemetry", IP2STR(&ip_info.gw));
    return (ret > 0 && ret < (int)max_len);
}

esp_err_t send_telemetry_to_backend(const char *backend_ip, uint16_t port, const telemetry_data_t *data) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/telemetry", backend_ip, port);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", data->device_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)data->timestamp);
    cJSON_AddNumberToObject(root, "temperature", data->temperature);
    cJSON_AddNumberToObject(root, "humidity", data->humidity);
    cJSON_AddNumberToObject(root, "gas_voc", data->gas_voc);
    cJSON_AddNumberToObject(root, "pressure", data->pressure);

    char *json_string = cJSON_PrintUnformatted(root);
    if (json_string == NULL) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_string, strlen(json_string));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Ingest status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Ingest failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    free(json_string);

    return err;
}

void WiFi_Server_Task(void *pvParameters) {
    QueueHandle_t data_queue = (QueueHandle_t)pvParameters;
    telemetry_data_t telemetry;
    char backend_url[128];

    for (;;) {
        if (xQueueReceive(data_queue, &telemetry, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!get_backend_url(backend_url, sizeof(backend_url))) {
            ESP_LOGE(TAG, "Failed to resolve backend gateway IP. Skipping dispatch.");
            continue;
        }

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device_id", telemetry.device_id);
        cJSON_AddNumberToObject(root, "timestamp", (double)telemetry.timestamp);
        cJSON_AddNumberToObject(root, "temperature", telemetry.temperature);
        cJSON_AddNumberToObject(root, "humidity", telemetry.humidity);
        cJSON_AddNumberToObject(root, "gas_voc", telemetry.gas_voc);
        cJSON_AddNumberToObject(root, "pressure", telemetry.pressure);

        char *json_string = cJSON_PrintUnformatted(root);
        if (json_string == NULL) {
            cJSON_Delete(root);
            continue;
        }

        esp_http_client_config_t config = {
            .url = backend_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 4000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize esp_http_client instance");
            cJSON_Delete(root);
            free(json_string);
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_string, strlen(json_string));

        ESP_LOGI(TAG, "Dispatching telemetry to: %s", backend_url);
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP POST completed. Status: %d", status_code);
        } else {
            ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        cJSON_Delete(root);
        free(json_string);
    }
}