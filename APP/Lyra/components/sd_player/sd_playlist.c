#include "sd_player.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <esp_log.h>

static const char *TAG = "playlist";

static const char *AUDIO_EXTENSIONS[] = { ".wav", ".flac", ".mp3", NULL };

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
