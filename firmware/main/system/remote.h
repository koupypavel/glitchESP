/*
 * Serial remote control (development aid): text commands on the console UART so the camera
 * can be driven and checked from a PC without touching it. Type "help" for the list.
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*remote_action_t)(void *user);

/* `photo` and `video` are the same handlers the BOOT button calls. */
esp_err_t remote_init(remote_action_t photo, remote_action_t video, void *user);

#ifdef __cplusplus
}
#endif
