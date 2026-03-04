/*
 * lib_storage.c — JSON persistence on SD card for Lyra library.
 *
 * Files:
 *   /sdcard/.lyra/favorites.json
 *   /sdcard/.lyra/history.json
 *   /sdcard/.lyra/playlists/index.json
 *   /sdcard/.lyra/playlists/pl_0.json .. pl_15.json
 *
 * JSON format uses short keys to minimize file size:
 *   s=source, t=title, a=artist, al=album, d=duration_ms, ts=timestamp, p=path/id/url
 */

#include "lib_storage.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "lib_store";

#define LYRA_DIR       "/sdcard/.lyra"
#define PLAYLISTS_DIR  "/sdcard/.lyra/playlists"
#define FAV_PATH       LYRA_DIR "/favorites.json"
#define HIST_PATH      LYRA_DIR "/history.json"
#define PL_INDEX_PATH  PLAYLISTS_DIR "/index.json"

//--------------------------------------------------------------------+
// Helpers: directory creation
//--------------------------------------------------------------------+

static void mkdir_if_needed(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0775);
    }
}

esp_err_t lib_storage_ensure_dirs(void)
{
    mkdir_if_needed(LYRA_DIR);
    mkdir_if_needed(PLAYLISTS_DIR);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Helpers: read/write file with PSRAM buffer
//--------------------------------------------------------------------+

static char *read_file_psram(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 512 * 1024) {
        fclose(f);
        return NULL;
    }

    char *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, len, f);
    fclose(f);
    buf[read] = '\0';
    if (out_len) *out_len = read;
    return buf;
}

static esp_err_t write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

//--------------------------------------------------------------------+
// Helpers: JSON <-> lib_track_t conversion
//--------------------------------------------------------------------+

static cJSON *track_to_json(const lib_track_t *t)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "s", (int)t->source);
    cJSON_AddStringToObject(obj, "t", t->title);
    cJSON_AddStringToObject(obj, "a", t->artist);
    cJSON_AddStringToObject(obj, "al", t->album);
    cJSON_AddNumberToObject(obj, "d", t->duration_ms);
    cJSON_AddNumberToObject(obj, "ts", t->timestamp);

    switch (t->source) {
    case LIB_SOURCE_SD:       cJSON_AddStringToObject(obj, "p", t->file_path); break;
    case LIB_SOURCE_SUBSONIC: cJSON_AddStringToObject(obj, "p", t->subsonic_id); break;
    case LIB_SOURCE_NET:      cJSON_AddStringToObject(obj, "p", t->url); break;
    }
    return obj;
}

static void json_to_track(const cJSON *obj, lib_track_t *t)
{
    memset(t, 0, sizeof(*t));

    cJSON *js = cJSON_GetObjectItem(obj, "s");
    if (js) t->source = (lib_source_t)js->valueint;

    cJSON *jt = cJSON_GetObjectItem(obj, "t");
    if (jt && jt->valuestring)
        strncpy(t->title, jt->valuestring, sizeof(t->title) - 1);

    cJSON *ja = cJSON_GetObjectItem(obj, "a");
    if (ja && ja->valuestring)
        strncpy(t->artist, ja->valuestring, sizeof(t->artist) - 1);

    cJSON *jal = cJSON_GetObjectItem(obj, "al");
    if (jal && jal->valuestring)
        strncpy(t->album, jal->valuestring, sizeof(t->album) - 1);

    cJSON *jd = cJSON_GetObjectItem(obj, "d");
    if (jd) t->duration_ms = (uint32_t)jd->valuedouble;

    cJSON *jts = cJSON_GetObjectItem(obj, "ts");
    if (jts) t->timestamp = (uint32_t)jts->valuedouble;

    cJSON *jp = cJSON_GetObjectItem(obj, "p");
    if (jp && jp->valuestring) {
        switch (t->source) {
        case LIB_SOURCE_SD:
            strncpy(t->file_path, jp->valuestring, sizeof(t->file_path) - 1);
            break;
        case LIB_SOURCE_SUBSONIC:
            strncpy(t->subsonic_id, jp->valuestring, sizeof(t->subsonic_id) - 1);
            break;
        case LIB_SOURCE_NET:
            strncpy(t->url, jp->valuestring, sizeof(t->url) - 1);
            break;
        }
    }
}

//--------------------------------------------------------------------+
// Generic: load/save track array from/to JSON file
//--------------------------------------------------------------------+

static esp_err_t load_tracks_from_file(const char *path, lib_track_t *out, int max, int *count)
{
    *count = 0;
    size_t len = 0;
    char *data = read_file_psram(path, &len);
    if (!data) return ESP_OK;  // File doesn't exist yet — empty is OK

    cJSON *arr = cJSON_Parse(data);
    free(data);

    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return ESP_OK;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > max) n = max;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (item) json_to_track(item, &out[i]);
    }
    *count = n;

    cJSON_Delete(arr);
    ESP_LOGI(TAG, "Loaded %d tracks from %s", n, path);
    return ESP_OK;
}

static esp_err_t save_tracks_to_file(const char *path, const lib_track_t *tracks, int count)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, track_to_json(&tracks[i]));
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t ret = write_file(path, json, strlen(json));
    cJSON_free(json);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Saved %d tracks to %s", count, path);
    }
    return ret;
}

//--------------------------------------------------------------------+
// Favorites
//--------------------------------------------------------------------+

esp_err_t lib_storage_load_favorites(lib_track_t *out, int max, int *count)
{
    return load_tracks_from_file(FAV_PATH, out, max, count);
}

esp_err_t lib_storage_save_favorites(const lib_track_t *tracks, int count)
{
    return save_tracks_to_file(FAV_PATH, tracks, count);
}

//--------------------------------------------------------------------+
// History
//--------------------------------------------------------------------+

esp_err_t lib_storage_load_history(lib_track_t *out, int max, int *count)
{
    return load_tracks_from_file(HIST_PATH, out, max, count);
}

esp_err_t lib_storage_save_history(const lib_track_t *tracks, int count)
{
    return save_tracks_to_file(HIST_PATH, tracks, count);
}

//--------------------------------------------------------------------+
// Playlist index
//--------------------------------------------------------------------+

esp_err_t lib_storage_load_playlist_index(lib_playlist_info_t *out, int max, int *count)
{
    *count = 0;
    size_t len = 0;
    char *data = read_file_psram(PL_INDEX_PATH, &len);
    if (!data) return ESP_OK;

    cJSON *arr = cJSON_Parse(data);
    free(data);

    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return ESP_OK;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > max) n = max;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        memset(&out[i], 0, sizeof(out[i]));

        cJSON *jn = cJSON_GetObjectItem(item, "name");
        if (jn && jn->valuestring)
            strncpy(out[i].name, jn->valuestring, sizeof(out[i].name) - 1);

        cJSON *jid = cJSON_GetObjectItem(item, "id");
        if (jid) out[i].id = (uint8_t)jid->valueint;

        cJSON *jc = cJSON_GetObjectItem(item, "count");
        if (jc) out[i].track_count = jc->valueint;
    }
    *count = n;

    cJSON_Delete(arr);
    ESP_LOGI(TAG, "Loaded %d playlists from index", n);
    return ESP_OK;
}

esp_err_t lib_storage_save_playlist_index(const lib_playlist_info_t *playlists, int count)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", playlists[i].name);
        cJSON_AddNumberToObject(obj, "id", playlists[i].id);
        cJSON_AddNumberToObject(obj, "count", playlists[i].track_count);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t ret = write_file(PL_INDEX_PATH, json, strlen(json));
    cJSON_free(json);
    return ret;
}

//--------------------------------------------------------------------+
// Individual playlist tracks
//--------------------------------------------------------------------+

static void playlist_file_path(uint8_t id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, PLAYLISTS_DIR "/pl_%d.json", id);
}

esp_err_t lib_storage_load_playlist_tracks(uint8_t id, lib_track_t *out, int max, int *count)
{
    char path[64];
    playlist_file_path(id, path, sizeof(path));
    return load_tracks_from_file(path, out, max, count);
}

esp_err_t lib_storage_save_playlist_tracks(uint8_t id, const lib_track_t *tracks, int count)
{
    char path[64];
    playlist_file_path(id, path, sizeof(path));
    return save_tracks_to_file(path, tracks, count);
}

esp_err_t lib_storage_delete_playlist_file(uint8_t id)
{
    char path[64];
    playlist_file_path(id, path, sizeof(path));
    remove(path);
    return ESP_OK;
}
