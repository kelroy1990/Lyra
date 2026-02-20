#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

//--------------------------------------------------------------------+
// DLNA/UPnP Renderer public API
//--------------------------------------------------------------------+

// Transport states (UPnP AVTransport spec)
typedef enum {
    DLNA_TRANSPORT_STOPPED,
    DLNA_TRANSPORT_PLAYING,
    DLNA_TRANSPORT_PAUSED,
    DLNA_TRANSPORT_TRANSITIONING,
    DLNA_TRANSPORT_NO_MEDIA,
} dlna_transport_state_t;

// Control callbacks from UPnP controller to application
typedef struct {
    // Controller calls Play (url = stream URL to open)
    void (*on_play)(const char *url, const char *metadata);

    // Controller calls Pause
    void (*on_pause)(void);

    // Controller calls Stop
    void (*on_stop)(void);

    // Controller calls Seek (target_ms = target position in milliseconds)
    void (*on_seek)(uint32_t target_ms);

    // Controller calls SetVolume (volume 0-100)
    void (*on_volume)(uint8_t volume);

    // Controller calls SetMute
    void (*on_mute)(bool mute);
} dlna_control_cbs_t;

//--------------------------------------------------------------------+
// Lifecycle
//--------------------------------------------------------------------+

// Start DLNA renderer. device_name = friendly name visible to controllers (e.g. "Lyra").
// Registers SSDP/UPnP services; requires WiFi to be connected first.
esp_err_t dlna_renderer_start(const char *device_name);

// Stop DLNA renderer and free resources.
void dlna_renderer_stop(void);

//--------------------------------------------------------------------+
// Control interface (called by app_main callbacks, registered at start)
//--------------------------------------------------------------------+

void dlna_register_callbacks(const dlna_control_cbs_t *cbs);

//--------------------------------------------------------------------+
// Status feedback (called by net_audio task → informs UPnP controller)
//--------------------------------------------------------------------+

// Update current playback position (call periodically while playing)
void dlna_set_position(uint32_t current_ms, uint32_t total_ms);

// Update UPnP transport state (call when playback state changes)
void dlna_set_transport_state(dlna_transport_state_t state);

//--------------------------------------------------------------------+
// Status queries
//--------------------------------------------------------------------+

bool dlna_is_active(void);   // True if renderer running and controller connected

// Notify controller we paused (e.g. user switched to SD source)
void dlna_notify_pause(void);

// Notify controller we resumed
void dlna_notify_play(void);
