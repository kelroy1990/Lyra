#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Track source types (aligned with qm_source_t)
//--------------------------------------------------------------------+

typedef enum {
    LIB_SOURCE_SD = 0,
    LIB_SOURCE_SUBSONIC,
    LIB_SOURCE_NET,
} lib_source_t;

//--------------------------------------------------------------------+
// Track entry (used for favorites, playlists, history)
//--------------------------------------------------------------------+

typedef struct {
    lib_source_t source;
    char     title[128];
    char     artist[64];
    char     album[128];
    uint32_t duration_ms;
    uint32_t timestamp;      // Unix epoch: when added/played
    union {
        char file_path[256];     // LIB_SOURCE_SD
        char subsonic_id[48];    // LIB_SOURCE_SUBSONIC
        char url[256];           // LIB_SOURCE_NET
    };
} lib_track_t;

//--------------------------------------------------------------------+
// Limits
//--------------------------------------------------------------------+

#define LIB_MAX_FAVORITES    500
#define LIB_MAX_PLAYLISTS     16
#define LIB_MAX_PL_TRACKS    200
#define LIB_MAX_HISTORY      200
#define LIB_PLAYLIST_NAME_LEN 48

//--------------------------------------------------------------------+
// Playlist info (lightweight, always cached)
//--------------------------------------------------------------------+

typedef struct {
    char    name[LIB_PLAYLIST_NAME_LEN];
    uint8_t id;               // 0-15
    int     track_count;
} lib_playlist_info_t;

//--------------------------------------------------------------------+
// API: Init / Persistence
//--------------------------------------------------------------------+

// Load library from SD card JSON files into PSRAM cache.
// Creates /sdcard/.lyra/ directory if it doesn't exist.
esp_err_t library_init(void);

// Flush dirty data to SD card (favorites + history).
esp_err_t lib_save(void);

// Save a specific playlist's tracks to SD.
esp_err_t lib_save_playlist(uint8_t id);

//--------------------------------------------------------------------+
// API: Favorites
//--------------------------------------------------------------------+

esp_err_t lib_favorite_add(const lib_track_t *track);
esp_err_t lib_favorite_remove(int index);
bool      lib_is_favorite(const char *title, const char *artist);
int       lib_favorite_count(void);
const lib_track_t *lib_favorite_get(int index);

// Enqueue all favorites into queue_manager
void      lib_favorite_play_all(void);

//--------------------------------------------------------------------+
// API: Playlists
//--------------------------------------------------------------------+

esp_err_t lib_playlist_create(const char *name);
esp_err_t lib_playlist_delete(uint8_t id);
esp_err_t lib_playlist_rename(uint8_t id, const char *new_name);
esp_err_t lib_playlist_add_track(uint8_t id, const lib_track_t *track);
esp_err_t lib_playlist_remove_track(uint8_t id, int track_index);
int       lib_playlist_count(void);
const lib_playlist_info_t *lib_playlist_get_info(int index);

// Load tracks of a playlist (caller provides buffer).
// Returns ESP_OK, sets *count to number of tracks loaded.
esp_err_t lib_playlist_load_tracks(uint8_t id, lib_track_t *out, int max, int *count);

// Enqueue entire playlist into queue_manager
void      lib_playlist_play(uint8_t id);

//--------------------------------------------------------------------+
// API: History (automatic, circular)
//--------------------------------------------------------------------+

void      lib_history_record(const lib_track_t *track);
int       lib_history_count(void);
const lib_track_t *lib_history_get(int index);  // 0 = most recent
void      lib_history_clear(void);

//--------------------------------------------------------------------+
// CDC command handler
//--------------------------------------------------------------------+

typedef void (*lib_print_fn_t)(const char *fmt, ...);
void lib_handle_cdc_command(const char *sub, lib_print_fn_t print);

#ifdef __cplusplus
}
#endif

#endif /* LIBRARY_H */
