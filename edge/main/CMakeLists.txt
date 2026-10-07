idf_component_register(
    SRCS 
        "src/main.c"
        "src/wifi.c"
        "src/system_monitor.c"
        "src/lcd_task.c"
        "src/camera_task.c"
        "test/test.c"
        "test/lcd_min_test.c"
    INCLUDE_DIRS 
        "include"
        "src"
        "test"
    REQUIRES 
        freertos
        log
        esp_system
        cjson
        mbedtls
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
        esp_lvgl_port
        lvgl
        esp32-camera
)

set(ENV_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../.env")
if(EXISTS "${ENV_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ENV_FILE}")
    file(STRINGS "${ENV_FILE}" ENV_LINES REGEX "^[ \t]*[A-Za-z0-9_]+[ \t]*=")
    foreach(line ${ENV_LINES})
        if(line MATCHES "^([^=]+)=(.*)$")
            string(STRIP "${CMAKE_MATCH_1}" KEY)
            string(STRIP "${CMAKE_MATCH_2}" VAL)
            string(REGEX REPLACE "^[\"']|[\"']$" "" VAL "${VAL}")
            if(KEY STREQUAL "WIFI_SSID")
                set(CONFIG_ESP_WIFI_SSID "${VAL}")
            elseif(KEY STREQUAL "WIFI_PASS")
                set(CONFIG_ESP_WIFI_PASSWORD "${VAL}")
            elseif(KEY STREQUAL "BACKEND_IP")
                set(CONFIG_BACKEND_IP "${VAL}")
            elseif(KEY STREQUAL "BACKEND_PORT")
                set(CONFIG_BACKEND_PORT "${VAL}")
            endif()
        endif()
    endforeach()
endif()

# Compile definitions loaded from .env file
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

# Enable DEBUG log level for this component (Level 4 = DEBUG)
target_compile_definitions(${COMPONENT_LIB} PRIVATE 
    LOG_LOCAL_LEVEL=ESP_LOG_DEBUG
)

# C standards and debug flags
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-ggdb3" "-Og")