#include "sd_player.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <esp_log.h>

static const char *TAG = "playlist";

static const char *AUDIO_EXTENSIONS[] = { ".wav", ".flac", ".mp3", ".cue", NULL };

static bool is_audio_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;

    for (int i = 0; AUDIO_EXTENSIONS[i]; i++) {
        if (strcasecmp(dot, AUDIO_EXTENSIONS[i]) == 0) return true;
    }
    return false;
}

static int cmp_strings(const void *a, const void *b)
{
    return strcasecmp(*(const char **)a, *(const char **)b);
}

// Check if name is a .cue file
static bool is_cue_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".cue") == 0;
}

// Check if an audio file has a matching .cue in the list → should be hidden
static bool has_matching_cue(const char *audio_name, char **names, int count)
{
    const char *dot = strrchr(audio_name, '.');
    if (!dot) return false;
    size_t base_len = dot - audio_name;

    for (int i = 0; i < count; i++) {
        if (!names[i]) continue;
        const char *cdot = strrchr(names[i], '.');
        if (!cdot || strcasecmp(cdot, ".cue") != 0) continue;
        size_t cbase_len = cdot - names[i];
        if (cbase_len == base_len && strncasecmp(audio_name, names[i], base_len) == 0) {
            return true;
        }
    }
    return false;
}

int sd_playlist_scan(const char *folder_path, char **names_out, int max_tracks)
{
    DIR *dir = opendir(folder_path);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open folder: %s", folder_path);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_tracks) {
        // Skip hidden files and directories
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type == DT_DIR) continue;

        if (is_audio_file(entry->d_name)) {
            names_out[count] = strdup(entry->d_name);
            if (names_out[count]) {
                count++;
            }
        }
    }
    closedir(dir);

    // Dedup: if album.cue exists, hide album.wav/album.flac/album.mp3
    for (int i = 0; i < count; i++) {
        if (!is_cue_file(names_out[i]) && has_matching_cue(names_out[i], names_out, count)) {
            free(names_out[i]);
            names_out[i] = names_out[count - 1];
            names_out[count - 1] = NULL;
            count--;
            i--;  // re-check swapped entry
        }
    }

    ESP_LOGI(TAG, "Scanned %s: %d audio files", folder_path, count);
    return count;
}

void sd_playlist_free(char **names, int count)
{
    for (int i = 0; i < count; i++) {
        free(names[i]);
        names[i] = NULL;
    }
}

void sd_playlist_sort(char **names, int count)
{
    if (count > 1) {
        qsort(names, count, sizeof(char *), cmp_strings);
    }
}
