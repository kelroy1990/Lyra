#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "usb_descriptors.h"

// Initialize mode manager (call before tusb_init)
void usb_mode_init(usb_mode_t initial_mode);

// Get current USB mode
usb_mode_t usb_mode_get(void);

// Switch USB mode: disconnects, changes descriptors, reconnects
// Blocks ~700ms during transition. CDC connection will be lost.
esp_err_t usb_mode_switch(usb_mode_t new_mode);

// Audio task suspend/resume (called internally by usb_mode_switch)
void audio_task_set_active(bool active);
bool audio_task_is_active(void);
