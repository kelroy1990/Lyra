#pragma once

#include <stdbool.h>
#include "esp_err.h"

//--------------------------------------------------------------------+
// Subsonic / Navidrome REST API client
// Uses token-based auth (v1.13.0+), JSON responses, esp_http_client.
// Streaming goes through net_audio_cmd_start().
//--------------------------------------------------------------------+

typedef enum {
    SUBSONIC_DISCONNECTED,
    SUBSONIC_CONNECTING,
    SUBSONIC_CONNECTED,
    SUBSONIC_ERROR,
} subsonic_state_t;

// Printf-style callback (same pattern as spotify_print_fn_t)
typedef void (*subsonic_print_fn_t)(const char *fmt, ...);

// Initialize subsonic module (call once at startup)
void subsonic_init(void);

// Connect to a Subsonic/Navidrome server (blocking — makes HTTP ping).
// server_url: "http://192.168.1.100:4533" (no trailing slash)
// Returns ESP_OK on successful ping.
esp_err_t subsonic_connect(const char *server_url, const char *user,
                            const char *password, subsonic_print_fn_t print);

// Retry connect using already-stored credentials (e.g. after WiFi comes up).
// Only acts if state is ERROR or CONNECTING (credentials were set but ping failed).
esp_err_t subsonic_reconnect(void);

// Query state
subsonic_state_t subsonic_get_state(void);
const char *subsonic_get_server_url(void);
const char *subsonic_get_username(void);

// Set max bitrate for transcoding (0=native, 128/192/256/320)
void subsonic_set_max_bitrate(uint16_t kbps);
uint16_t subsonic_get_max_bitrate(void);

// Build a stream URL for a given track ID (uses current auth credentials).
// Writes into url_buf. Returns ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
esp_err_t subsonic_build_stream_url(const char *track_id, char *url_buf, size_t buf_size);

// CDC command handler (dispatches all "subsonic <subcommand>" commands).
// subcommand is everything after "subsonic " (e.g. "albums newest").
void subsonic_handle_cdc_command(const char *subcommand,
                                  subsonic_print_fn_t print);
