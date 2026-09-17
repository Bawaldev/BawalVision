#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DEVICE_ID_MAX_LEN     64
#define DEVICE_NAME_MAX_CHAR  100
#define FW_VERSION_MAX_LEN    54


typedef struct __attribute__((__packed__)) {
    char     device_id[DEVICE_ID_MAX_LEN];
    char     firmware_version[FW_VERSION_MAX_LEN];
    int64_t  timestamp;    
    float    temperature;    
    float    humidity;      
    float    gas_voc;        
    float    pressure; 
    uint8_t  current_cpu_load; 
    char     device_name[DEVICE_NAME_MAX_CHAR];
    uint8_t  device_core;
} telemetry_data_t;

typedef struct __attribute__((__packed__))  {
    telemetry_data_t telemetry;
    uint8_t         *image_buf;
    size_t           image_len;
} inspection_payload_t;