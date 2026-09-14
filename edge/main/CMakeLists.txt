idf_component_register(
    SRCS 
        "src/main.c"
        "src/wifi.c"
        
    INCLUDE_DIRS 
        "include"
        "src"
    REQUIRES 
        esp_http_client
        esp_wifi
        nvs_flash
        esp_timer
        driver
)