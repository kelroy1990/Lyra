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

// Query state
subsonic_state_t subsonic_get_state(void);
const char *subsonic_get_server_url(void);
const char *subsonic_get_username(void);

// CDC command handler (dispatches all "subsonic <subcommand>" commands).
// subcommand is everything after "subsonic " (e.g. "albums newest").
void subsonic_handle_cdc_command(const char *subcommand,
                                  subsonic_print_fn_t print);
