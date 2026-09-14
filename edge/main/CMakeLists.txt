idf_component_register(
    SRCS 
        "src/main.c"
        "src/wifi.c"
        "test/test.c"
    INCLUDE_DIRS 
        "include"
        "src"
        "test"
    REQUIRES 
        cjson
        esp_wifi
        esp_event
        nvs_flash
        esp_netif
        esp_http_client
        esp_timer
        driver
)

if(DEFINED CONFIG_ESP_WIFI_SSID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ESP_WIFI_SSID="${CONFIG_ESP_WIFI_SSID}")
endif()

if(DEFINED CONFIG_ESP_WIFI_PASSWORD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ESP_WIFI_PASSWORD="${CONFIG_ESP_WIFI_PASSWORD}")
endif()

target_compile_features(${COMPONENT_LIB} PRIVATE c_std_17)