idf_component_register(
    SRCS 
        "src/main.c"
        "src/wifi.c"
        "src/system_monitor.c"
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
        esp_hw_support
        app_update
        esp_lcd
)

if(DEFINED CONFIG_ESP_WIFI_SSID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ESP_WIFI_SSID="${CONFIG_ESP_WIFI_SSID}")
endif()

if(DEFINED CONFIG_ESP_WIFI_PASSWORD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ESP_WIFI_PASSWORD="${CONFIG_ESP_WIFI_PASSWORD}")
endif()

if(DEFINED CONFIG_BACKEND_IP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_BACKEND_IP="${CONFIG_BACKEND_IP}")
endif()

if(DEFINED CONFIG_BACKEND_PORT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_BACKEND_PORT="${CONFIG_BACKEND_PORT}")
endif()

target_compile_features(${COMPONENT_LIB} PRIVATE c_std_17)

target_compile_options(${COMPONENT_LIB} PRIVATE "-ggdb3" "-Og")
