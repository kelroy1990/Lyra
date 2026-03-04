#ifndef LIB_STORAGE_H
#define LIB_STORAGE_H

/*
 * lib_storage.h — Internal: JSON persistence for library data.
 *
 * All files live under /sdcard/.lyra/ on the SD card.
 * Uses cJSON for serialization and PSRAM buffers for I/O.
 */

#include "library.h"

// Create /sdcard/.lyra/ and /sdcard/.lyra/playlists/ if they don't exist.
esp_err_t lib_storage_ensure_dirs(void);

// Favorites
esp_err_t lib_storage_load_favorites(lib_track_t *out, int max, int *count);
esp_err_t lib_storage_save_favorites(const lib_track_t *tracks, int count);

// History
esp_err_t lib_storage_load_history(lib_track_t *out, int max, int *count);
esp_err_t lib_storage_save_history(const lib_track_t *tracks, int count);

// Playlist index
esp_err_t lib_storage_load_playlist_index(lib_playlist_info_t *out, int max, int *count);
esp_err_t lib_storage_save_playlist_index(const lib_playlist_info_t *playlists, int count);

// Individual playlist tracks
esp_err_t lib_storage_load_playlist_tracks(uint8_t id, lib_track_t *out, int max, int *count);
esp_err_t lib_storage_save_playlist_tracks(uint8_t id, const lib_track_t *tracks, int count);
esp_err_t lib_storage_delete_playlist_file(uint8_t id);

#endif /* LIB_STORAGE_H */
