#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

//--------------------------------------------------------------------+
// Net audio state
//--------------------------------------------------------------------+

typedef enum {
    NET_AUDIO_IDLE,
    NET_AUDIO_CONNECTING,
    NET_AUDIO_BUFFERING,   // HTTP connected, filling pre-buffer
    NET_AUDIO_PLAYING,
    NET_AUDIO_PAUSED,
    NET_AUDIO_ERROR,
} net_audio_state_t;

//--------------------------------------------------------------------+
// Callbacks to audio system (same pattern as sd_player_audio_cbs_t)
//--------------------------------------------------------------------+

typedef struct {
    int  (*get_source)(void);
    void (*switch_source)(int new_source, uint32_t sample_rate, uint8_t bits);
    void (*set_producer_handle)(TaskHandle_t handle);
    StreamBufferHandle_t (*get_stream_buffer)(void);
    void (*process_audio)(int32_t *buffer, uint32_t frames);

    // Source IDs — must match audio_source_t enum in main/audio_source.h.
    // Passed here to avoid a component→main include dependency.
    int audio_source_none;  // AUDIO_SOURCE_NONE value
    int audio_source_net;   // AUDIO_SOURCE_NET  value

    // Called once during init to register net pause/resume callbacks.
    // Signature matches audio_source_register_net_cbs().
    void (*register_source_cbs)(void (*pause_cb)(void), void (*resume_cb)(void));
} net_audio_audio_cbs_t;

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

// Initialize net_audio subsystem. Call once at startup, after audio system is ready.
// Registers NET pause/resume callbacks with audio_source automatically.
void net_audio_init(const net_audio_audio_cbs_t *cbs);

// Create the net_audio task (call after net_audio_init)
void net_audio_start_task(void);

// Start streaming from URL.
// codec_hint: "flac", "mp3", "wav", "aac", "ogg", or NULL (autodetect from Content-Type).
// referer:    Referer header value, or NULL. Required by some CDNs (Radio France,
//             BBC, etc.) that enforce hotlink protection with HTTP 403.
//             Example: "https://www.radiofrance.fr/"
// Returns ESP_OK if command accepted (not connected yet).
esp_err_t net_audio_cmd_start(const char *url, const char *codec_hint,
                               const char *referer);

// Stop current stream, switch source to NONE
void net_audio_cmd_stop(void);

// Pause consumption without closing HTTP connection
// (Called automatically by audio_source when switching away from NET)
void net_audio_cmd_pause(void);

// Resume consumption after pause
// (Called automatically by audio_source when switching back to NET)
void net_audio_cmd_resume(void);

// Query current state (thread-safe)
net_audio_state_t net_audio_get_state(void);

// True if actively connected and playing or paused
bool net_audio_is_active(void);

// Current stream info (only valid while active)
typedef struct {
    char url[512];
    char content_type[64];
    char codec[16];        // "flac", "mp3", "wav", "aac", "ogg"
    char icy_title[256];   // ICY StreamTitle (internet radio), or ""
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint8_t  channels;
    uint32_t bitrate_kbps; // Estimated bitrate (0 if unknown)
    uint32_t elapsed_ms;
} net_audio_info_t;

net_audio_info_t net_audio_get_info(void);
