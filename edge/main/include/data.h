#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DEVICE_ID_MAX_LEN 32

typedef struct {
    char     device_id[DEVICE_ID_MAX_LEN];
    int64_t  timestamp;    
    float    temperature;    
    float    humidity;      
    float    gas_voc;        
    float    pressure; 
    uint8_t  current_cpu_load; 
} telemetry_data_t;

typedef struct {
    telemetry_data_t telemetry;
    uint8_t         *image_buf;
    size_t           image_len;
} inspection_payload_t;