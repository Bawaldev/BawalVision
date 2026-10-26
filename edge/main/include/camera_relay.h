#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_KEY_RELAY_URL "relay_url"
#define NVS_KEY_RELAY_KEY "relay_key"
#define NVS_KEY_RELAY_FPS "relay_fps"

#define RELAY_URL_MAX 192
#define RELAY_KEY_MAX 96
#define RELAY_DEFAULT_FPS 15
#define RELAY_MAX_FPS 30

void camera_relay_start(void);

bool camera_relay_set_config(const char *url, const char *key, int fps);

bool camera_relay_get_config(char *url, size_t url_len,
                             char *key, size_t key_len,
                             int *fps);

bool camera_relay_is_connected(void);
uint32_t camera_relay_frames_sent(void);
uint32_t camera_relay_send_errors(void);
int64_t camera_relay_last_frame_ms(void);

#ifdef __cplusplus
}
#endif
