#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "audio_codecs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

//--------------------------------------------------------------------+
// Player states
//--------------------------------------------------------------------+

typedef enum {
    PLAYER_STATE_IDLE,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
} player_state_t;

//--------------------------------------------------------------------+
// Audio source enum (mirrors main/audio_source.h to avoid dependency)
//--------------------------------------------------------------------+

typedef enum {
    SD_AUDIO_SOURCE_NONE = 0,
    SD_AUDIO_SOURCE_USB  = 1,
    SD_AUDIO_SOURCE_SD   = 2,
} sd_audio_source_t;

//--------------------------------------------------------------------+
// Callbacks to audio system (avoids circular dependency with main)
//--------------------------------------------------------------------+

typedef struct {
    // Get current audio source
    int (*get_source)(void);
    // Switch audio source (source, sample_rate, bits). Pass 0,0 to keep format.
    void (*switch_source)(int new_source, uint32_t sample_rate, uint8_t bits);
    // Register the active producer task handle for feeder notifications
    void (*set_producer_handle)(TaskHandle_t handle);
    // Get the shared stream buffer
    StreamBufferHandle_t (*get_stream_buffer)(void);
    // Process audio through DSP chain (int32_t stereo interleaved)
    void (*process_audio)(int32_t *buffer, uint32_t frames);
} sd_player_audio_cbs_t;

//--------------------------------------------------------------------+
// Player status (read-only snapshot)
//--------------------------------------------------------------------+

typedef struct {
    player_state_t state;
    char current_file[160];
    codec_info_t file_info;
    uint64_t current_frame;
    uint32_t elapsed_ms;
    uint32_t remaining_ms;
    int      track_index;
    int      track_count;
} player_status_t;

//--------------------------------------------------------------------+
// Output function type (for CDC printing)
//--------------------------------------------------------------------+

typedef void (*player_output_fn)(const char *fmt, ...);

//--------------------------------------------------------------------+
// Initialization
//--------------------------------------------------------------------+

// Initialize the SD player subsystem. Call once at startup.
void sd_player_init(player_output_fn output_fn, const sd_player_audio_cbs_t *audio_cbs);

// Create the player task (call after sd_player_init)
void sd_player_start_task(void);

//--------------------------------------------------------------------+
// Playback commands (thread-safe, called from CDC task)
//--------------------------------------------------------------------+

void sd_player_cmd_play(const char *path);
void sd_player_cmd_pause(void);
void sd_player_cmd_resume(void);
void sd_player_cmd_stop(void);
void sd_player_cmd_next(void);
void sd_player_cmd_prev(void);
void sd_player_cmd_seek(uint32_t seconds);

//--------------------------------------------------------------------+
// Status queries (thread-safe)
//--------------------------------------------------------------------+

player_status_t sd_player_get_status(void);
bool sd_player_is_active(void);

//--------------------------------------------------------------------+
// Info commands (print to CDC)
//--------------------------------------------------------------------+

void sd_player_cmd_track_info(void);
void sd_player_cmd_playlist_info(void);

//--------------------------------------------------------------------+
// Playlist
//--------------------------------------------------------------------+

#define PLAYLIST_MAX_TRACKS 200

int sd_playlist_scan(const char *folder_path, char **names_out, int max_tracks);
void sd_playlist_free(char **names, int count);
void sd_playlist_sort(char **names, int count);
