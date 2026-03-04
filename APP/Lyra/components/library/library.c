/*
 * library.c — Favorites, playlists, and history for Lyra.
 *
 * Storage: JSON on SD card (/sdcard/.lyra/) + PSRAM cache for fast access.
 * Auto-save: dirty flag + periodic timer (60s) to avoid excessive SD writes.
 */

#include "library.h"
#include "lib_storage.h"
#include "queue_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "library";

//--------------------------------------------------------------------+
// PSRAM-backed cache
//--------------------------------------------------------------------+

static struct {
    lib_track_t       *favorites;     // PSRAM array, max LIB_MAX_FAVORITES
    int                fav_count;
    bool               fav_dirty;

    lib_playlist_info_t playlists[LIB_MAX_PLAYLISTS];
    int                pl_count;
    bool               pl_dirty;

    lib_track_t       *history;       // PSRAM array, max LIB_MAX_HISTORY
    int                hist_count;
    bool               hist_dirty;

    bool               loaded;
    esp_timer_handle_t save_timer;
} s_lib;

//--------------------------------------------------------------------+
// Auto-save timer callback (runs every 60s)
//--------------------------------------------------------------------+

static void autosave_cb(void *arg)
{
    (void)arg;
    if (s_lib.fav_dirty || s_lib.hist_dirty || s_lib.pl_dirty) {
        lib_save();
    }
}

//--------------------------------------------------------------------+
// Init
//--------------------------------------------------------------------+

esp_err_t library_init(void)
{
    if (s_lib.loaded) return ESP_OK;

    // Ensure directories exist
    lib_storage_ensure_dirs();

    // Allocate PSRAM arrays
    s_lib.favorites = heap_caps_calloc(LIB_MAX_FAVORITES, sizeof(lib_track_t), MALLOC_CAP_SPIRAM);
    if (!s_lib.favorites) s_lib.favorites = calloc(LIB_MAX_FAVORITES, sizeof(lib_track_t));
    if (!s_lib.favorites) {
        ESP_LOGE(TAG, "OOM: favorites (%d bytes)", (int)(LIB_MAX_FAVORITES * sizeof(lib_track_t)));
        return ESP_ERR_NO_MEM;
    }

    s_lib.history = heap_caps_calloc(LIB_MAX_HISTORY, sizeof(lib_track_t), MALLOC_CAP_SPIRAM);
    if (!s_lib.history) s_lib.history = calloc(LIB_MAX_HISTORY, sizeof(lib_track_t));
    if (!s_lib.history) {
        ESP_LOGE(TAG, "OOM: history (%d bytes)", (int)(LIB_MAX_HISTORY * sizeof(lib_track_t)));
        return ESP_ERR_NO_MEM;
    }

    // Load from SD
    lib_storage_load_favorites(s_lib.favorites, LIB_MAX_FAVORITES, &s_lib.fav_count);
    lib_storage_load_history(s_lib.history, LIB_MAX_HISTORY, &s_lib.hist_count);
    lib_storage_load_playlist_index(s_lib.playlists, LIB_MAX_PLAYLISTS, &s_lib.pl_count);

    // Start auto-save timer (60 seconds)
    esp_timer_create_args_t timer_args = {
        .callback = autosave_cb,
        .name = "lib_save",
    };
    esp_timer_create(&timer_args, &s_lib.save_timer);
    esp_timer_start_periodic(s_lib.save_timer, 60 * 1000000ULL);  // 60s

    s_lib.loaded = true;
    ESP_LOGI(TAG, "Library loaded: %d favorites, %d playlists, %d history",
             s_lib.fav_count, s_lib.pl_count, s_lib.hist_count);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Save (flush dirty data to SD)
//--------------------------------------------------------------------+

esp_err_t lib_save(void)
{
    if (s_lib.fav_dirty) {
        lib_storage_save_favorites(s_lib.favorites, s_lib.fav_count);
        s_lib.fav_dirty = false;
    }
    if (s_lib.hist_dirty) {
        lib_storage_save_history(s_lib.history, s_lib.hist_count);
        s_lib.hist_dirty = false;
    }
    if (s_lib.pl_dirty) {
        lib_storage_save_playlist_index(s_lib.playlists, s_lib.pl_count);
        s_lib.pl_dirty = false;
    }
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Favorites
//--------------------------------------------------------------------+

esp_err_t lib_favorite_add(const lib_track_t *track)
{
    if (!track || s_lib.fav_count >= LIB_MAX_FAVORITES) {
        return (s_lib.fav_count >= LIB_MAX_FAVORITES) ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }

    // Check for duplicates
    if (lib_is_favorite(track->title, track->artist)) {
        ESP_LOGW(TAG, "Already in favorites: %s", track->title);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&s_lib.favorites[s_lib.fav_count], track, sizeof(lib_track_t));
    // Set timestamp if not provided
    if (s_lib.favorites[s_lib.fav_count].timestamp == 0) {
        s_lib.favorites[s_lib.fav_count].timestamp = (uint32_t)time(NULL);
    }
    s_lib.fav_count++;
    s_lib.fav_dirty = true;

    ESP_LOGI(TAG, "Favorite added [%d]: %s — %s", s_lib.fav_count, track->title, track->artist);
    return ESP_OK;
}

esp_err_t lib_favorite_remove(int index)
{
    if (index < 0 || index >= s_lib.fav_count) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Favorite removed [%d]: %s", index + 1, s_lib.favorites[index].title);

    memmove(&s_lib.favorites[index], &s_lib.favorites[index + 1],
            (s_lib.fav_count - index - 1) * sizeof(lib_track_t));
    s_lib.fav_count--;
    s_lib.fav_dirty = true;
    return ESP_OK;
}

bool lib_is_favorite(const char *title, const char *artist)
{
    if (!title) return false;
    for (int i = 0; i < s_lib.fav_count; i++) {
        if (strcmp(s_lib.favorites[i].title, title) == 0) {
            if (!artist || !artist[0] || strcmp(s_lib.favorites[i].artist, artist) == 0) {
                return true;
            }
        }
    }
    return false;
}

int lib_favorite_count(void) { return s_lib.fav_count; }

const lib_track_t *lib_favorite_get(int index)
{
    if (index < 0 || index >= s_lib.fav_count) return NULL;
    return &s_lib.favorites[index];
}

void lib_favorite_play_all(void)
{
    if (s_lib.fav_count == 0) return;

    qm_clear();
    for (int i = 0; i < s_lib.fav_count; i++) {
        qm_track_t qt = {0};
        qt.source = (qm_source_t)s_lib.favorites[i].source;
        strncpy(qt.title, s_lib.favorites[i].title, sizeof(qt.title) - 1);
        strncpy(qt.artist, s_lib.favorites[i].artist, sizeof(qt.artist) - 1);
        strncpy(qt.album, s_lib.favorites[i].album, sizeof(qt.album) - 1);
        qt.duration_ms = s_lib.favorites[i].duration_ms;
        memcpy(&qt.file_path, &s_lib.favorites[i].file_path, sizeof(qt.file_path));
        qm_append(&qt);
    }
    qm_play();
    ESP_LOGI(TAG, "Playing all favorites (%d tracks)", s_lib.fav_count);
}

//--------------------------------------------------------------------+
// Playlists
//--------------------------------------------------------------------+

static int find_free_playlist_id(void)
{
    bool used[LIB_MAX_PLAYLISTS] = {0};
    for (int i = 0; i < s_lib.pl_count; i++) {
        if (s_lib.playlists[i].id < LIB_MAX_PLAYLISTS)
            used[s_lib.playlists[i].id] = true;
    }
    for (int i = 0; i < LIB_MAX_PLAYLISTS; i++) {
        if (!used[i]) return i;
    }
    return -1;
}

esp_err_t lib_playlist_create(const char *name)
{
    if (!name || !*name) return ESP_ERR_INVALID_ARG;
    if (s_lib.pl_count >= LIB_MAX_PLAYLISTS) return ESP_ERR_NO_MEM;

    int id = find_free_playlist_id();
    if (id < 0) return ESP_ERR_NO_MEM;

    lib_playlist_info_t *pl = &s_lib.playlists[s_lib.pl_count];
    memset(pl, 0, sizeof(*pl));
    strncpy(pl->name, name, sizeof(pl->name) - 1);
    pl->id = (uint8_t)id;
    pl->track_count = 0;
    s_lib.pl_count++;
    s_lib.pl_dirty = true;

    ESP_LOGI(TAG, "Playlist created: [%d] \"%s\"", id, name);
    return ESP_OK;
}

esp_err_t lib_playlist_delete(uint8_t id)
{
    int idx = -1;
    for (int i = 0; i < s_lib.pl_count; i++) {
        if (s_lib.playlists[i].id == id) { idx = i; break; }
    }
    if (idx < 0) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "Playlist deleted: [%d] \"%s\"", id, s_lib.playlists[idx].name);

    lib_storage_delete_playlist_file(id);
    memmove(&s_lib.playlists[idx], &s_lib.playlists[idx + 1],
            (s_lib.pl_count - idx - 1) * sizeof(lib_playlist_info_t));
    s_lib.pl_count--;
    s_lib.pl_dirty = true;
    return ESP_OK;
}

esp_err_t lib_playlist_rename(uint8_t id, const char *new_name)
{
    if (!new_name || !*new_name) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < s_lib.pl_count; i++) {
        if (s_lib.playlists[i].id == id) {
            strncpy(s_lib.playlists[i].name, new_name, sizeof(s_lib.playlists[i].name) - 1);
            s_lib.pl_dirty = true;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t lib_playlist_add_track(uint8_t id, const lib_track_t *track)
{
    if (!track) return ESP_ERR_INVALID_ARG;

    // Find playlist info
    lib_playlist_info_t *info = NULL;
    for (int i = 0; i < s_lib.pl_count; i++) {
        if (s_lib.playlists[i].id == id) { info = &s_lib.playlists[i]; break; }
    }
    if (!info) return ESP_ERR_NOT_FOUND;
    if (info->track_count >= LIB_MAX_PL_TRACKS) return ESP_ERR_NO_MEM;

    // Load existing tracks, append, save
    lib_track_t *tracks = heap_caps_malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t), MALLOC_CAP_SPIRAM);
    if (!tracks) tracks = malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t));
    if (!tracks) return ESP_ERR_NO_MEM;

    int count = 0;
    lib_storage_load_playlist_tracks(id, tracks, LIB_MAX_PL_TRACKS, &count);

    memcpy(&tracks[count], track, sizeof(lib_track_t));
    if (tracks[count].timestamp == 0) tracks[count].timestamp = (uint32_t)time(NULL);
    count++;

    esp_err_t ret = lib_storage_save_playlist_tracks(id, tracks, count);
    free(tracks);

    if (ret == ESP_OK) {
        info->track_count = count;
        s_lib.pl_dirty = true;
        ESP_LOGI(TAG, "Added to playlist [%d]: %s", id, track->title);
    }
    return ret;
}

esp_err_t lib_playlist_remove_track(uint8_t id, int track_index)
{
    // Find playlist info
    lib_playlist_info_t *info = NULL;
    for (int i = 0; i < s_lib.pl_count; i++) {
        if (s_lib.playlists[i].id == id) { info = &s_lib.playlists[i]; break; }
    }
    if (!info) return ESP_ERR_NOT_FOUND;

    lib_track_t *tracks = heap_caps_malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t), MALLOC_CAP_SPIRAM);
    if (!tracks) tracks = malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t));
    if (!tracks) return ESP_ERR_NO_MEM;

    int count = 0;
    lib_storage_load_playlist_tracks(id, tracks, LIB_MAX_PL_TRACKS, &count);

    if (track_index < 0 || track_index >= count) {
        free(tracks);
        return ESP_ERR_INVALID_ARG;
    }

    memmove(&tracks[track_index], &tracks[track_index + 1],
            (count - track_index - 1) * sizeof(lib_track_t));
    count--;

    esp_err_t ret = lib_storage_save_playlist_tracks(id, tracks, count);
    free(tracks);

    if (ret == ESP_OK) {
        info->track_count = count;
        s_lib.pl_dirty = true;
    }
    return ret;
}

int lib_playlist_count(void) { return s_lib.pl_count; }

const lib_playlist_info_t *lib_playlist_get_info(int index)
{
    if (index < 0 || index >= s_lib.pl_count) return NULL;
    return &s_lib.playlists[index];
}

esp_err_t lib_playlist_load_tracks(uint8_t id, lib_track_t *out, int max, int *count)
{
    return lib_storage_load_playlist_tracks(id, out, max, count);
}

esp_err_t lib_save_playlist(uint8_t id)
{
    // Playlist tracks are saved immediately on add/remove, this is a no-op
    // but we save the index to be safe
    return lib_storage_save_playlist_index(s_lib.playlists, s_lib.pl_count);
}

void lib_playlist_play(uint8_t id)
{
    lib_track_t *tracks = heap_caps_malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t), MALLOC_CAP_SPIRAM);
    if (!tracks) tracks = malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t));
    if (!tracks) return;

    int count = 0;
    lib_storage_load_playlist_tracks(id, tracks, LIB_MAX_PL_TRACKS, &count);

    if (count > 0) {
        qm_clear();
        for (int i = 0; i < count; i++) {
            qm_track_t qt = {0};
            qt.source = (qm_source_t)tracks[i].source;
            strncpy(qt.title, tracks[i].title, sizeof(qt.title) - 1);
            strncpy(qt.artist, tracks[i].artist, sizeof(qt.artist) - 1);
            strncpy(qt.album, tracks[i].album, sizeof(qt.album) - 1);
            qt.duration_ms = tracks[i].duration_ms;
            memcpy(&qt.file_path, &tracks[i].file_path, sizeof(qt.file_path));
            qm_append(&qt);
        }
        qm_play();
        ESP_LOGI(TAG, "Playing playlist [%d] (%d tracks)", id, count);
    }

    free(tracks);
}

//--------------------------------------------------------------------+
// History (circular buffer in PSRAM)
//--------------------------------------------------------------------+

void lib_history_record(const lib_track_t *track)
{
    if (!track || !s_lib.history) return;

    // Shift everything down by 1 (newest at index 0)
    if (s_lib.hist_count < LIB_MAX_HISTORY) {
        memmove(&s_lib.history[1], &s_lib.history[0],
                s_lib.hist_count * sizeof(lib_track_t));
        s_lib.hist_count++;
    } else {
        memmove(&s_lib.history[1], &s_lib.history[0],
                (LIB_MAX_HISTORY - 1) * sizeof(lib_track_t));
    }

    memcpy(&s_lib.history[0], track, sizeof(lib_track_t));
    if (s_lib.history[0].timestamp == 0) {
        s_lib.history[0].timestamp = (uint32_t)time(NULL);
    }
    s_lib.hist_dirty = true;
}

int lib_history_count(void) { return s_lib.hist_count; }

const lib_track_t *lib_history_get(int index)
{
    if (index < 0 || index >= s_lib.hist_count) return NULL;
    return &s_lib.history[index];
}

void lib_history_clear(void)
{
    s_lib.hist_count = 0;
    s_lib.hist_dirty = true;
    ESP_LOGI(TAG, "History cleared");
}

//--------------------------------------------------------------------+
// CDC command handler
//--------------------------------------------------------------------+

void lib_handle_cdc_command(const char *sub, lib_print_fn_t print)
{
    if (!sub || !*sub) {
        print("Usage: lib <fav|pl|history|save|stats>\r\n");
        return;
    }

    // ── Favorites ──
    if (strncmp(sub, "fav ", 4) == 0) {
        const char *arg = sub + 4;

        if (strcmp(arg, "add") == 0) {
            // "lib fav add" — add currently playing track
            // This needs to be wired from app_main with current track info
            print("Use from app_main (auto-detects current track)\r\n");
            return;
        }

        if (strncmp(arg, "remove ", 7) == 0) {
            int idx = atoi(arg + 7) - 1;  // 1-based
            if (lib_favorite_remove(idx) == ESP_OK) {
                print("Removed favorite #%d\r\n", idx + 1);
            } else {
                print("Invalid index (1-%d)\r\n", s_lib.fav_count);
            }
            return;
        }

        if (strcmp(arg, "list") == 0) {
            if (s_lib.fav_count == 0) {
                print("No favorites\r\n");
                return;
            }
            static const char *src_names[] = {"SD", "SUB", "NET"};
            for (int i = 0; i < s_lib.fav_count; i++) {
                const lib_track_t *t = &s_lib.favorites[i];
                print("  [%d] [%s] %s", i + 1, src_names[t->source], t->title);
                if (t->artist[0]) print(" — %s", t->artist);
                print("\r\n");
            }
            print("Total: %d favorites\r\n", s_lib.fav_count);
            return;
        }

        if (strcmp(arg, "play") == 0) {
            if (s_lib.fav_count == 0) {
                print("No favorites to play\r\n");
                return;
            }
            lib_favorite_play_all();
            print("Playing %d favorites\r\n", s_lib.fav_count);
            return;
        }

        print("Usage: lib fav <add|remove N|list|play>\r\n");
        return;
    }

    // ── Playlists ──
    if (strncmp(sub, "pl ", 3) == 0) {
        const char *arg = sub + 3;

        if (strncmp(arg, "create ", 7) == 0) {
            const char *name = arg + 7;
            if (lib_playlist_create(name) == ESP_OK) {
                print("Playlist created: \"%s\"\r\n", name);
            } else {
                print("Error creating playlist (max %d)\r\n", LIB_MAX_PLAYLISTS);
            }
            return;
        }

        if (strncmp(arg, "delete ", 7) == 0) {
            uint8_t id = (uint8_t)atoi(arg + 7);
            if (lib_playlist_delete(id) == ESP_OK) {
                print("Playlist %d deleted\r\n", id);
            } else {
                print("Playlist %d not found\r\n", id);
            }
            return;
        }

        if (strcmp(arg, "list") == 0) {
            if (s_lib.pl_count == 0) {
                print("No playlists\r\n");
                return;
            }
            for (int i = 0; i < s_lib.pl_count; i++) {
                print("  [%d] \"%s\" (%d tracks)\r\n",
                      s_lib.playlists[i].id,
                      s_lib.playlists[i].name,
                      s_lib.playlists[i].track_count);
            }
            return;
        }

        if (strncmp(arg, "show ", 5) == 0) {
            uint8_t id = (uint8_t)atoi(arg + 5);
            lib_track_t *tracks = heap_caps_malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t), MALLOC_CAP_SPIRAM);
            if (!tracks) tracks = malloc(LIB_MAX_PL_TRACKS * sizeof(lib_track_t));
            if (!tracks) { print("OOM\r\n"); return; }

            int count = 0;
            lib_storage_load_playlist_tracks(id, tracks, LIB_MAX_PL_TRACKS, &count);
            if (count == 0) {
                print("Playlist %d is empty\r\n", id);
            } else {
                for (int i = 0; i < count; i++) {
                    print("  [%d] %s", i + 1, tracks[i].title);
                    if (tracks[i].artist[0]) print(" — %s", tracks[i].artist);
                    print("\r\n");
                }
            }
            free(tracks);
            return;
        }

        if (strncmp(arg, "add ", 4) == 0) {
            // "lib pl add <id>" — add current track (needs wiring from app_main)
            print("Use from app_main (auto-detects current track)\r\n");
            return;
        }

        if (strncmp(arg, "remove ", 7) == 0) {
            // "lib pl remove <id> <track_idx>"
            int id = 0, tidx = 0;
            if (sscanf(arg + 7, "%d %d", &id, &tidx) == 2) {
                if (lib_playlist_remove_track((uint8_t)id, tidx - 1) == ESP_OK) {
                    print("Removed track %d from playlist %d\r\n", tidx, id);
                } else {
                    print("Error removing track\r\n");
                }
            } else {
                print("Usage: lib pl remove <playlist_id> <track_number>\r\n");
            }
            return;
        }

        if (strncmp(arg, "play ", 5) == 0) {
            uint8_t id = (uint8_t)atoi(arg + 5);
            lib_playlist_play(id);
            print("Playing playlist %d\r\n", id);
            return;
        }

        print("Usage: lib pl <create|delete|list|show|add|remove|play> [args]\r\n");
        return;
    }

    // ── History ──
    if (strncmp(sub, "history", 7) == 0) {
        const char *arg = sub + 7;
        while (*arg == ' ') arg++;

        if (strcmp(arg, "clear") == 0) {
            lib_history_clear();
            print("History cleared\r\n");
            return;
        }

        // Default: show recent 20
        int show = s_lib.hist_count;
        if (show > 20) show = 20;
        if (show == 0) {
            print("No history\r\n");
            return;
        }
        for (int i = 0; i < show; i++) {
            const lib_track_t *t = &s_lib.history[i];
            print("  [%d] %s", i + 1, t->title);
            if (t->artist[0]) print(" — %s", t->artist);
            print("\r\n");
        }
        if (s_lib.hist_count > 20) {
            print("  ... and %d more\r\n", s_lib.hist_count - 20);
        }
        return;
    }

    // ── Save ──
    if (strcmp(sub, "save") == 0) {
        lib_save();
        print("Library saved to SD\r\n");
        return;
    }

    // ── Stats ──
    if (strcmp(sub, "stats") == 0) {
        print("Library stats:\r\n");
        print("  Favorites: %d / %d\r\n", s_lib.fav_count, LIB_MAX_FAVORITES);
        print("  Playlists: %d / %d\r\n", s_lib.pl_count, LIB_MAX_PLAYLISTS);
        print("  History:   %d / %d\r\n", s_lib.hist_count, LIB_MAX_HISTORY);
        size_t mem = (s_lib.fav_count * sizeof(lib_track_t)) +
                     (s_lib.hist_count * sizeof(lib_track_t)) +
                     sizeof(s_lib.playlists);
        print("  PSRAM used: %d KB\r\n", (int)(mem / 1024));
        print("  Dirty: fav=%d hist=%d pl=%d\r\n",
              s_lib.fav_dirty, s_lib.hist_dirty, s_lib.pl_dirty);
        return;
    }

    print("Unknown library command. Usage: lib <fav|pl|history|save|stats>\r\n");
}
