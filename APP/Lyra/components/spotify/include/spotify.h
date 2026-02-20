#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

//--------------------------------------------------------------------+
// Spotify Connect via cspot library
// F8-E — Requires cspot submódulo: git submodule add
//   https://github.com/feelfreelinux/cspot components/spotify/vendor/cspot
//--------------------------------------------------------------------+

typedef struct {
    bool     connected;
    char     track_name[256];
    char     artist_name[256];
    char     album_name[256];
    uint32_t duration_ms;
    uint32_t position_ms;
    bool     is_playing;
    bool     shuffle;
    bool     repeat;
    uint8_t  volume;            // 0-100
} spotify_status_t;

// Audio callbacks (same pattern as net_audio / sd_player)
typedef struct {
    int  (*get_source)(void);
    void (*switch_source)(int new_source, uint32_t sample_rate, uint8_t bits);
    void (*set_producer_handle)(TaskHandle_t handle);
    StreamBufferHandle_t (*get_stream_buffer)(void);
    void (*process_audio)(int32_t *buffer, uint32_t frames);
} spotify_audio_cbs_t;

//--------------------------------------------------------------------+
// API
//--------------------------------------------------------------------+

// Initialize Spotify Connect.
// device_name: visible name in Spotify app (e.g. "Lyra").
// Requires cspot library present in vendor/cspot.
esp_err_t spotify_init(const char *device_name, const spotify_audio_cbs_t *cbs);

// Start Spotify Connect service (announces via ZeroConf/mDNS).
// Returns ESP_ERR_NOT_SUPPORTED if cspot is not yet integrated.
esp_err_t spotify_start(void);

// Stop service and release resources
void spotify_stop(void);

// Transport control (called by user or audio_source switch)
void spotify_send_pause(void);
void spotify_send_play(void);

// Query state
bool spotify_is_active(void);
spotify_status_t spotify_get_status(void);

// Clear stored credentials (forces re-authentication on next connect)
esp_err_t spotify_logout(void);

// CDC command handler
typedef void (*spotify_print_fn_t)(const char *fmt, ...);
void spotify_handle_cdc_command(const char *subcommand, spotify_print_fn_t print_fn);
