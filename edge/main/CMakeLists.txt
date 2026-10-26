idf_component_register(
    SRCS 
        "src/main.c"
        "src/wifi.c"
        "src/system_monitor.c"
        "src/lcd_task.c"
        "src/camera_task.c"
        "src/camera_relay.c"
    INCLUDE_DIRS 
        "include"
        "src"
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
        esp_http_server
        esp_websocket_client
        esp_timer
        driver
        esp_hw_support
        app_update
        esp_lcd
        esp_lvgl_port
        lvgl
        esp32-camera
        esp_lcd_st7735
        esp_driver_tsens
        esp_driver_usb_serial_jtag
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
            elseif(KEY STREQUAL "BAWAL_DEVICE_TOKEN")
                set(CONFIG_DEVICE_AUTH_TOKEN "${VAL}")
            elseif(KEY STREQUAL "CAMERA_RELAY_URL")
                set(CONFIG_CAMERA_RELAY_URL "${VAL}")
            elseif(KEY STREQUAL "CAMERA_RELAY_KEY")
                set(CONFIG_CAMERA_RELAY_KEY "${VAL}")
            elseif(KEY STREQUAL "RELAY_INGEST_KEY")
                set(CONFIG_CAMERA_RELAY_KEY "${VAL}")
            elseif(KEY STREQUAL "CAMERA_RELAY_FPS")
                set(CONFIG_CAMERA_RELAY_FPS "${VAL}")
            elseif(KEY STREQUAL "CAMERA_RELAY_DEVICE")
                set(CONFIG_CAMERA_RELAY_DEVICE "${VAL}")
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

if(DEFINED CONFIG_DEVICE_AUTH_TOKEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_DEVICE_AUTH_TOKEN="${CONFIG_DEVICE_AUTH_TOKEN}")
endif()

if(DEFINED CONFIG_CAMERA_RELAY_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_CAMERA_RELAY_URL="${CONFIG_CAMERA_RELAY_URL}")
endif()

if(DEFINED CONFIG_CAMERA_RELAY_KEY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_CAMERA_RELAY_KEY="${CONFIG_CAMERA_RELAY_KEY}")
endif()

if(DEFINED CONFIG_CAMERA_RELAY_FPS AND NOT CONFIG_CAMERA_RELAY_FPS STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_CAMERA_RELAY_FPS=${CONFIG_CAMERA_RELAY_FPS})
endif()

if(DEFINED CONFIG_CAMERA_RELAY_DEVICE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_CAMERA_RELAY_DEVICE="${CONFIG_CAMERA_RELAY_DEVICE}")
endif()

# Enable DEBUG log level for this component (Level 4 = DEBUG)
target_compile_definitions(${COMPONENT_LIB} PRIVATE 
    LOG_LOCAL_LEVEL=ESP_LOG_DEBUG
)

# C standards and debug flags
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-ggdb3" "-Og")
