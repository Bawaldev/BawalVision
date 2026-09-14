#pragma once
#include <stdint.h>
#include "data.h"


#define DEVICE_ID_MAX_LEN 32

void initWiFiServer();
void WiFi_Server_Task(void *pvParameters);
