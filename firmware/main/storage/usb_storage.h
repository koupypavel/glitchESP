/*
 * USB storage mode: the microSD card handed to a computer as a USB drive over the board's
 * USB OTG port (high speed).
 *
 * A card can have one owner at a time, so this is a mode and not a background service:
 * start() takes the card away from the camera (no photos, no gallery) and gives it, as raw
 * sectors, to the TinyUSB mass-storage class; stop() takes it back and mounts it again.
 * The computer should eject the drive before stop(), like any card reader.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_STORAGE_OFF,
    USB_STORAGE_WAITING,        /* on, no computer on the OTG port (yet) */
    USB_STORAGE_CONNECTED,      /* the computer has the drive */
    USB_STORAGE_EJECTED,        /* the computer ejected it: safe to leave */
} usb_storage_state_t;

/* ESP_ERR_INVALID_STATE while a photo or a recording is in progress, ESP_ERR_NOT_FOUND
 * without a card. */
esp_err_t usb_storage_start(void);
void usb_storage_stop(void);
bool usb_storage_active(void);
usb_storage_state_t usb_storage_state(void);

#ifdef __cplusplus
}
#endif
