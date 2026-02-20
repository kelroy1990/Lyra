#include "metadata.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "lwip/sockets.h"    // lwip_getaddrinfo — required with LWIP_COMPAT_SOCKETS=0

static const char *TAG = "metadata";

#define META_SUBDIR      ".meta"
#define META_ALBUM_FILE  "album.json"
#define COVER_FILENAME   "cover.jpg"
#define NVS_PARTITION    "lyra_cfg"
#define NVS_NAMESPACE    "meta"

//--------------------------------------------------------------------+
// Rate limiting state
//--------------------------------------------------------------------+

static uint64_t s_last_mb_req_us   = 0;  // Last MusicBrainz request time
static uint64_t s_last_ca_req_us   = 0;  // Last Cover Art Archive request
static uint64_t s_last_lf_req_us   = 0;  // Last Last.fm request

#define MB_MIN_INTERVAL_US   1100000ULL  // 1.1 seconds
#define CA_MIN_INTERVAL_US    200000ULL  // 200ms
#define LF_MIN_INTERVAL_US    200000ULL  // 200ms

static volatile bool     s_cancel_flag    = false;
static volatile uint8_t  s_progress       = 0;

static void rate_limit_wait(uint64_t *last_us, uint64_t min_interval_us)
{
    uint64_t now = esp_timer_get_time();
    if (now - *last_us < min_interval_us) {
        uint64_t wait_us = min_interval_us - (now - *last_us);
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000 + 1));
    }
    *last_us = esp_timer_get_time();
}

//--------------------------------------------------------------------+
// Simple HTTP GET → return allocated body (caller must free)
//--------------------------------------------------------------------+

static char *http_get_json(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url         = url,
        .timeout_ms  = 20000,
        .buffer_size = 4096,
        .user_agent  = "Lyra-Player/1.0 (github.com/lyra-player/lyra)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;

    esp_http_client_set_header(client, "Accept", "application/json");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    int64_t clen = esp_http_client_fetch_headers(client);
    int status   = esp_http_client_get_status_code(client);

    if (status < 200 || status >= 300 || clen <= 0 || clen > 65536) {
        ESP_LOGD(TAG, "HTTP %d, clen=%lld for %s", status, clen, url);
        esp_http_client_cleanup(client);
        return NULL;
    }

    char *body = malloc((size_t)clen + 1);
    if (!body) { esp_http_client_cleanup(client); return NULL; }

    int total = 0;
    while (total < (int)clen) {
        int rd = esp_http_client_read(client, body + total, (int)clen - total);
        if (rd <= 0) break;
        total += rd;
    }
    body[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (out_len) *out_len = (size_t)total;
    return body;
}

//--------------------------------------------------------------------+
// HTTP download binary to file
//--------------------------------------------------------------------+

static esp_err_t http_download_file(const char *url, const char *filepath)
{
    esp_http_client_config_t cfg = {
        .url         = url,
        .timeout_ms  = 30000,
        .buffer_size = 8192,
        .user_agent  = "Lyra-Player/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int64_t clen = esp_http_client_fetch_headers(client);
    int status   = esp_http_client_get_status_code(client);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create: %s", filepath);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char buf[4096];
    int64_t total = 0;
    int rd;
    while ((rd = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, rd, f);
        total += rd;
        if (clen > 0 && total > clen) break;
    }
    fclose(f);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Downloaded %lld bytes -> %s", total, filepath);
    return (total > 0) ? ESP_OK : ESP_FAIL;
}

//--------------------------------------------------------------------+
// Filesystem helpers
//--------------------------------------------------------------------+

static esp_err_t mkdir_p(const char *path)
{
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? ESP_OK : ESP_FAIL;
}

static void path_join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

//--------------------------------------------------------------------+
// ID3v2 tag parser (minimal — title, artist, album, date, track)
//--------------------------------------------------------------------+

static void parse_id3v2(FILE *f, lyra_track_meta_t *meta)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10) return;
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return;

    uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                        ((uint32_t)(hdr[7] & 0x7F) << 14) |
                        ((uint32_t)(hdr[8] & 0x7F) <<  7) |
                        ((uint32_t)(hdr[9] & 0x7F));

    uint32_t consumed = 0;
    uint8_t frame_hdr[10];

    while (consumed < tag_size) {
        if (fread(frame_hdr, 1, 10, f) != 10) break;
        consumed += 10;

        uint32_t fsize = ((uint32_t)frame_hdr[4] << 24) |
                         ((uint32_t)frame_hdr[5] << 16) |
                         ((uint32_t)frame_hdr[6] <<  8) |
                         ((uint32_t)frame_hdr[7]);

        if (fsize == 0 || fsize > 65536) break;

        char frame_id[5] = {0};
        memcpy(frame_id, frame_hdr, 4);

        char *val = malloc(fsize + 1);
        if (!val) break;
        if ((uint32_t)fread(val, 1, fsize, f) != fsize) { free(val); break; }
        val[fsize] = '\0';
        consumed += fsize;

        // Skip encoding byte (val[0] = encoding)
        const char *text = (fsize > 1) ? val + 1 : val;

        if      (strcmp(frame_id, "TIT2") == 0) strncpy(meta->title, text, sizeof(meta->title) - 1);
        else if (strcmp(frame_id, "TPE1") == 0) strncpy(meta->artist, text, sizeof(meta->artist) - 1);
        else if (strcmp(frame_id, "TALB") == 0) strncpy(meta->album, text, sizeof(meta->album) - 1);
        else if (strcmp(frame_id, "TPE2") == 0) strncpy(meta->album_artist, text, sizeof(meta->album_artist) - 1);
        else if (strcmp(frame_id, "TDRC") == 0 ||
                 strcmp(frame_id, "TYER") == 0) strncpy(meta->date, text, sizeof(meta->date) - 1);
        else if (strcmp(frame_id, "TCON") == 0) strncpy(meta->genre, text, sizeof(meta->genre) - 1);
        else if (strcmp(frame_id, "TRCK") == 0) meta->track_number = atoi(text);
        else if (strcmp(frame_id, "TPOS") == 0) meta->disc_number = atoi(text);
        else if (strcmp(frame_id, "TXXX") == 0) {
            // Embedded MusicBrainz ID: description + value separated by null byte
            const char *desc = text;
            const char *mbval = desc + strlen(desc) + 1;
            if (strcmp(desc, "MusicBrainz Release Id") == 0)
                strncpy(meta->musicbrainz_release_id, mbval, sizeof(meta->musicbrainz_release_id) - 1);
            else if (strcmp(desc, "MusicBrainz Track Id") == 0)
                strncpy(meta->musicbrainz_track_id, mbval, sizeof(meta->musicbrainz_track_id) - 1);
        }
        else if (strcmp(frame_id, "TSRC") == 0) strncpy(meta->isrc, text, sizeof(meta->isrc) - 1);
        else if (strcmp(frame_id, "APIC") == 0) meta->has_embedded_cover = true;

        free(val);
    }
}

//--------------------------------------------------------------------+
// VORBISCOMMENT parser (FLAC/Ogg — KEY=value pairs)
//--------------------------------------------------------------------+

static void set_vorbis_field(lyra_track_meta_t *meta, const char *key, const char *value)
{
    if (strcasecmp(key, "TITLE") == 0)        strncpy(meta->title, value, sizeof(meta->title) - 1);
    else if (strcasecmp(key, "ARTIST") == 0)       strncpy(meta->artist, value, sizeof(meta->artist) - 1);
    else if (strcasecmp(key, "ALBUM") == 0)        strncpy(meta->album, value, sizeof(meta->album) - 1);
    else if (strcasecmp(key, "ALBUMARTIST") == 0)  strncpy(meta->album_artist, value, sizeof(meta->album_artist) - 1);
    else if (strcasecmp(key, "DATE") == 0)         strncpy(meta->date, value, sizeof(meta->date) - 1);
    else if (strcasecmp(key, "GENRE") == 0)        strncpy(meta->genre, value, sizeof(meta->genre) - 1);
    else if (strcasecmp(key, "TRACKNUMBER") == 0)  meta->track_number = atoi(value);
    else if (strcasecmp(key, "DISCNUMBER") == 0)   meta->disc_number = atoi(value);
    else if (strcasecmp(key, "TOTALTRACKS") == 0)  meta->total_tracks = atoi(value);
    else if (strcasecmp(key, "TOTALDISCS") == 0)   meta->total_discs = atoi(value);
    else if (strcasecmp(key, "ISRC") == 0)         strncpy(meta->isrc, value, sizeof(meta->isrc) - 1);
    else if (strcasecmp(key, "MUSICBRAINZ_ALBUMID") == 0)
        strncpy(meta->musicbrainz_release_id, value, sizeof(meta->musicbrainz_release_id) - 1);
    else if (strcasecmp(key, "MUSICBRAINZ_TRACKID") == 0)
        strncpy(meta->musicbrainz_track_id, value, sizeof(meta->musicbrainz_track_id) - 1);
}

// FLAC VORBISCOMMENT block: 4-byte LE comment count, then 4-byte LE length + UTF-8 string each
static void parse_vorbiscomment_block(FILE *f, uint32_t block_len, lyra_track_meta_t *meta)
{
    uint32_t vendor_len;
    if (fread(&vendor_len, 4, 1, f) != 1) return;
    fseek(f, vendor_len, SEEK_CUR);

    uint32_t count;
    if (fread(&count, 4, 1, f) != 1) return;

    for (uint32_t i = 0; i < count && i < 128; i++) {
        uint32_t comment_len;
        if (fread(&comment_len, 4, 1, f) != 1) break;
        if (comment_len == 0 || comment_len > 4096) { fseek(f, comment_len, SEEK_CUR); continue; }

        char *comment = malloc(comment_len + 1);
        if (!comment) break;
        if ((uint32_t)fread(comment, 1, comment_len, f) != comment_len) { free(comment); break; }
        comment[comment_len] = '\0';

        char *eq = strchr(comment, '=');
        if (eq) {
            *eq = '\0';
            set_vorbis_field(meta, comment, eq + 1);
        }
        free(comment);
    }
}

// Scan FLAC metadata blocks for VORBIS_COMMENT
static void parse_flac_tags(FILE *f, lyra_track_meta_t *meta)
{
    uint8_t sig[4];
    if (fread(sig, 1, 4, f) != 4) return;
    if (memcmp(sig, "fLaC", 4) != 0) return;

    while (1) {
        uint8_t block_hdr[4];
        if (fread(block_hdr, 1, 4, f) != 4) break;
        int last     = (block_hdr[0] >> 7) & 1;
        int type     = block_hdr[0] & 0x7F;
        uint32_t len = ((uint32_t)block_hdr[1] << 16) |
                       ((uint32_t)block_hdr[2] <<  8) |
                       block_hdr[3];
        if (type == 4) {  // VORBIS_COMMENT
            parse_vorbiscomment_block(f, len, meta);
        } else if (type == 6) {  // PICTURE
            meta->has_embedded_cover = true;
            fseek(f, len, SEEK_CUR);
        } else {
            fseek(f, len, SEEK_CUR);
        }
        if (last) break;
    }
}

//--------------------------------------------------------------------+
// Extension detection
//--------------------------------------------------------------------+

static const char *file_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

//--------------------------------------------------------------------+
// Public: read local tags
//--------------------------------------------------------------------+

esp_err_t meta_read_local(const char *filepath, lyra_track_meta_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *ext = file_ext(filepath);
    FILE *f = fopen(filepath, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    if (strcasecmp(ext, "mp3") == 0) {
        parse_id3v2(f, out);
        out->bits_per_sample = 16;
    } else if (strcasecmp(ext, "flac") == 0) {
        parse_flac_tags(f, out);
    } else if (strcasecmp(ext, "ogg") == 0) {
        // Skip Ogg page header (4+1+1+2+4+4+1 = 27 + segments bytes) then VORBISCOMMENT
        // For now: skip (complex Ogg framing, implement later)
        ESP_LOGD(TAG, "Ogg VORBISCOMMENT parsing: TODO");
    }

    fclose(f);

    // Check for external cover
    char dir[256] = {0};
    const char *last_slash = strrchr(filepath, '/');
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - filepath);
        if (dir_len < sizeof(dir)) {
            memcpy(dir, filepath, dir_len);
            dir[dir_len] = '\0';
        }
    }

    char cover_path[320];
    meta_get_cover_path(dir, cover_path, sizeof(cover_path));
    struct stat st;
    if (stat(cover_path, &st) == 0) {
        out->has_external_cover = true;
    }

    strncpy(out->meta_source, "local", sizeof(out->meta_source) - 1);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Cover path helper
//--------------------------------------------------------------------+

void meta_get_cover_path(const char *dir_path, char *out_path, size_t out_size)
{
    snprintf(out_path, out_size, "%s/%s", dir_path, COVER_FILENAME);
}

//--------------------------------------------------------------------+
// Check if metadata already downloaded
//--------------------------------------------------------------------+

bool meta_has_downloaded(const char *dir_path)
{
    char album_json[320];
    snprintf(album_json, sizeof(album_json), "%s/%s/%s",
             dir_path, META_SUBDIR, META_ALBUM_FILE);
    struct stat st;
    return stat(album_json, &st) == 0;
}

//--------------------------------------------------------------------+
// API key NVS management
//--------------------------------------------------------------------+

esp_err_t meta_set_api_key(meta_service_t service, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE,
                                             NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    const char *nvs_key = (service == META_SERVICE_LASTFM) ? "lf_key" : "acid_key";
    err = nvs_set_str(h, nvs_key, key);
    nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t meta_get_api_key(meta_service_t service, char *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE,
                                             NVS_READONLY, &h);
    if (err != ESP_OK) { buf[0] = '\0'; return err; }

    const char *nvs_key = (service == META_SERVICE_LASTFM) ? "lf_key" : "acid_key";
    err = nvs_get_str(h, nvs_key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) buf[0] = '\0';
    return err;
}

//--------------------------------------------------------------------+
// MusicBrainz REST API helpers
//--------------------------------------------------------------------+

// Build search URL and fetch recording info.
// Returns allocated JSON string (caller frees) or NULL.
static char *mb_search_recording(const char *artist, const char *title, const char *album)
{
    char url[1024];
    // URL-encode basic: replace space with + (simplified)
    char a[128] = {0}, t[128] = {0}, al[128] = {0};
    size_t i, j;
    for (i = 0, j = 0; artist[i] && j < sizeof(a) - 1; i++) {
        a[j++] = (artist[i] == ' ') ? '+' : artist[i];
    }
    for (i = 0, j = 0; title[i] && j < sizeof(t) - 1; i++) {
        t[j++] = (title[i] == ' ') ? '+' : title[i];
    }
    for (i = 0, j = 0; album[i] && j < sizeof(al) - 1; i++) {
        al[j++] = (album[i] == ' ') ? '+' : album[i];
    }

    snprintf(url, sizeof(url),
             "https://musicbrainz.org/ws/2/recording"
             "?query=artist:%s+recording:%s+release:%s&fmt=json&limit=5",
             a, t, al);

    rate_limit_wait(&s_last_mb_req_us, MB_MIN_INTERVAL_US);
    return http_get_json(url, NULL);
}

// Fetch release by MBID
static char *mb_get_release(const char *release_id)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://musicbrainz.org/ws/2/release/%s"
             "?inc=release-groups+genres+artists+recordings&fmt=json",
             release_id);

    rate_limit_wait(&s_last_mb_req_us, MB_MIN_INTERVAL_US);
    return http_get_json(url, NULL);
}

// Fetch Cover Art Archive for a release
static char *caa_get_cover_url(const char *release_id)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://coverartarchive.org/release/%s", release_id);

    rate_limit_wait(&s_last_ca_req_us, CA_MIN_INTERVAL_US);
    return http_get_json(url, NULL);
}

//--------------------------------------------------------------------+
// Download album metadata for a directory
//--------------------------------------------------------------------+

esp_err_t meta_download_album(const char *file_or_dir_path,
                               meta_progress_cb_t progress_cb)
{
    s_cancel_flag = false;
    s_progress = 0;
    if (progress_cb) progress_cb(0);

    // Resolve directory
    char dir[256] = {0};
    struct stat st_check;
    if (stat(file_or_dir_path, &st_check) == 0 && S_ISDIR(st_check.st_mode)) {
        strncpy(dir, file_or_dir_path, sizeof(dir) - 1);
    } else {
        const char *slash = strrchr(file_or_dir_path, '/');
        if (slash) {
            size_t len = (size_t)(slash - file_or_dir_path);
            if (len < sizeof(dir)) { memcpy(dir, file_or_dir_path, len); dir[len] = '\0'; }
        } else {
            strncpy(dir, "/sdcard", sizeof(dir) - 1);
        }
    }

    ESP_LOGI(TAG, "Downloading metadata for: %s", dir);

    // Find a representative audio file to read tags from
    char audio_file[320] = {0};
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *ext = file_ext(ent->d_name);
            if (strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "mp3") == 0 ||
                strcasecmp(ext, "wav") == 0  || strcasecmp(ext, "ogg") == 0) {
                snprintf(audio_file, sizeof(audio_file), "%s/%s", dir, ent->d_name);
                break;
            }
        }
        closedir(d);
    }

    if (!audio_file[0]) {
        ESP_LOGW(TAG, "No audio files found in %s", dir);
        return ESP_ERR_NOT_FOUND;
    }

    // Read local tags
    lyra_track_meta_t local_meta = {0};
    meta_read_local(audio_file, &local_meta);
    if (progress_cb) progress_cb(10);

    if (s_cancel_flag) return ESP_ERR_TIMEOUT;

    // Search MusicBrainz
    char *mb_json = NULL;
    char release_id[64] = {0};

    if (local_meta.musicbrainz_release_id[0]) {
        strncpy(release_id, local_meta.musicbrainz_release_id, sizeof(release_id) - 1);
        ESP_LOGI(TAG, "Using existing MBID: %s", release_id);
    } else if (local_meta.artist[0] && local_meta.title[0]) {
        mb_json = mb_search_recording(local_meta.artist, local_meta.title,
                                      local_meta.album[0] ? local_meta.album : "");
        if (mb_json) {
            cJSON *root = cJSON_Parse(mb_json);
            free(mb_json);
            mb_json = NULL;
            if (root) {
                cJSON *recordings = cJSON_GetObjectItem(root, "recordings");
                if (recordings && cJSON_IsArray(recordings) && cJSON_GetArraySize(recordings) > 0) {
                    cJSON *first = cJSON_GetArrayItem(recordings, 0);
                    cJSON *releases = cJSON_GetObjectItem(first, "releases");
                    if (releases && cJSON_IsArray(releases) && cJSON_GetArraySize(releases) > 0) {
                        cJSON *rel = cJSON_GetArrayItem(releases, 0);
                        cJSON *rel_id = cJSON_GetObjectItem(rel, "id");
                        if (rel_id && cJSON_IsString(rel_id)) {
                            strncpy(release_id, rel_id->valuestring, sizeof(release_id) - 1);
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
    }

    if (progress_cb) progress_cb(30);
    if (s_cancel_flag) return ESP_ERR_TIMEOUT;

    // Fetch full release info
    cJSON *release_root = NULL;
    if (release_id[0]) {
        char *rel_json = mb_get_release(release_id);
        if (rel_json) {
            release_root = cJSON_Parse(rel_json);
            free(rel_json);
        }
    }

    if (progress_cb) progress_cb(50);

    // Create .meta directory
    char meta_dir[320];
    snprintf(meta_dir, sizeof(meta_dir), "%s/%s", dir, META_SUBDIR);
    mkdir_p(meta_dir);

    // Write album.json
    if (release_root) {
        char album_path[320];
        path_join(album_path, sizeof(album_path), meta_dir, META_ALBUM_FILE);

        cJSON *out_json = cJSON_CreateObject();
        cJSON_AddStringToObject(out_json, "mb_release_id", release_id);

        // Extract album fields
        cJSON *title    = cJSON_GetObjectItem(release_root, "title");
        cJSON *date_obj = cJSON_GetObjectItem(release_root, "date");
        cJSON *label    = cJSON_GetObjectItem(release_root, "label-info");
        cJSON *country  = cJSON_GetObjectItem(release_root, "country");
        cJSON *artists  = cJSON_GetObjectItem(release_root, "artist-credit");

        if (title && cJSON_IsString(title))
            cJSON_AddStringToObject(out_json, "album", title->valuestring);
        if (date_obj && cJSON_IsString(date_obj))
            cJSON_AddStringToObject(out_json, "date", date_obj->valuestring);
        if (country && cJSON_IsString(country))
            cJSON_AddStringToObject(out_json, "country", country->valuestring);
        if (artists && cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
            cJSON *credit = cJSON_GetArrayItem(artists, 0);
            cJSON *artist_obj = cJSON_GetObjectItem(credit, "artist");
            cJSON *artist_name = artist_obj ? cJSON_GetObjectItem(artist_obj, "name") : NULL;
            if (artist_name && cJSON_IsString(artist_name)) {
                cJSON_AddStringToObject(out_json, "artist", artist_name->valuestring);
                cJSON_AddStringToObject(out_json, "album_artist", artist_name->valuestring);
            }
        }

        cJSON_AddStringToObject(out_json, "meta_source", "musicbrainz");
        cJSON_AddStringToObject(out_json, "lyra_meta_version", "1");

        char *json_str = cJSON_PrintUnformatted(out_json);
        if (json_str) {
            FILE *f = fopen(album_path, "w");
            if (f) { fputs(json_str, f); fclose(f); }
            free(json_str);
        }
        cJSON_Delete(out_json);
        cJSON_Delete(release_root);

        ESP_LOGI(TAG, "Wrote: %s", album_path);
    }

    if (progress_cb) progress_cb(65);
    if (s_cancel_flag) return ESP_ERR_TIMEOUT;

    // Download cover art
    char cover_path[320];
    meta_get_cover_path(dir, cover_path, sizeof(cover_path));

    struct stat cover_stat;
    bool has_cover = (stat(cover_path, &cover_stat) == 0 && cover_stat.st_size > 1000);

    if (!has_cover && release_id[0]) {
        char *caa_json = caa_get_cover_url(release_id);
        if (caa_json) {
            cJSON *caa_root = cJSON_Parse(caa_json);
            free(caa_json);
            if (caa_root) {
                cJSON *images = cJSON_GetObjectItem(caa_root, "images");
                if (images && cJSON_IsArray(images)) {
                    for (int i = 0; i < cJSON_GetArraySize(images); i++) {
                        cJSON *img = cJSON_GetArrayItem(images, i);
                        cJSON *front = cJSON_GetObjectItem(img, "front");
                        if (front && cJSON_IsTrue(front)) {
                            cJSON *img_url = cJSON_GetObjectItem(img, "image");
                            if (img_url && cJSON_IsString(img_url)) {
                                ESP_LOGI(TAG, "Downloading cover: %s", img_url->valuestring);
                                http_download_file(img_url->valuestring, cover_path);
                            }
                            break;
                        }
                    }
                }
                cJSON_Delete(caa_root);
            }
        }
    }

    if (progress_cb) progress_cb(90);
    s_progress = 90;

    // TODO: Per-track JSON (iterate directory, download recording info for each file)
    // For F8-D initial: album.json + cover.jpg are the priority

    s_progress = 100;
    if (progress_cb) progress_cb(100);
    ESP_LOGI(TAG, "Metadata download complete for: %s", dir);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Download single track
//--------------------------------------------------------------------+

esp_err_t meta_download_track(const char *filepath, meta_progress_cb_t progress_cb)
{
    // Get containing directory and call album download
    char dir[256] = {0};
    const char *slash = strrchr(filepath, '/');
    if (slash) {
        size_t len = (size_t)(slash - filepath);
        if (len < sizeof(dir)) { memcpy(dir, filepath, len); dir[len] = '\0'; }
    }
    return meta_download_album(dir, progress_cb);
}

//--------------------------------------------------------------------+
// Library-wide download (recursive)
//--------------------------------------------------------------------+

// Recursive helper
static esp_err_t download_dir_recursive(const char *path, bool force,
                                         uint32_t *total, uint32_t *done)
{
    DIR *d = opendir(path);
    if (!d) return ESP_ERR_NOT_FOUND;

    // Count albums first (for progress estimation)
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (ent->d_type == DT_DIR) (*total)++;
    }
    rewinddir(d);

    while ((ent = readdir(d)) != NULL) {
        if (s_cancel_flag) { closedir(d); return ESP_ERR_TIMEOUT; }
        if (ent->d_name[0] == '.') continue;

        char subpath[320];
        path_join(subpath, sizeof(subpath), path, ent->d_name);

        struct stat st;
        if (stat(subpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            // Check if this directory contains audio files (leaf album dir)
            bool has_audio = false;
            DIR *sub = opendir(subpath);
            if (sub) {
                struct dirent *se;
                while ((se = readdir(sub)) != NULL) {
                    const char *ext = file_ext(se->d_name);
                    if (strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "mp3") == 0 ||
                        strcasecmp(ext, "wav") == 0) {
                        has_audio = true;
                        break;
                    }
                }
                closedir(sub);
            }

            if (has_audio) {
                if (force || !meta_has_downloaded(subpath)) {
                    meta_download_album(subpath, NULL);
                    (*done)++;
                    s_progress = (uint8_t)((*done * 95) / (*total > 0 ? *total : 1));
                }
            } else {
                // Recurse into subdirectory (artist level)
                download_dir_recursive(subpath, force, total, done);
            }
        }
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t meta_download_library(const char *root_path, bool force,
                                 meta_progress_cb_t progress_cb)
{
    s_cancel_flag = false;
    s_progress = 0;
    if (progress_cb) progress_cb(0);

    ESP_LOGI(TAG, "Library download start: %s (force=%d)", root_path, force);
    uint32_t total = 0, done = 0;
    esp_err_t err = download_dir_recursive(root_path, force, &total, &done);

    s_progress = 100;
    if (progress_cb) progress_cb(100);
    return err;
}

//--------------------------------------------------------------------+
// Cancel + progress
//--------------------------------------------------------------------+

void meta_cancel_download(void)
{
    s_cancel_flag = true;
}

uint8_t meta_get_download_progress(void)
{
    return s_progress;
}

//--------------------------------------------------------------------+
// CDC command handler
//--------------------------------------------------------------------+

void meta_handle_cdc_command(const char *subcommand, meta_print_fn_t print_fn)
{
    while (*subcommand == ' ') subcommand++;

    if (strncmp(subcommand, "info ", 5) == 0) {
        const char *path = subcommand + 5;
        while (*path == ' ') path++;
        char full[256];
        snprintf(full, sizeof(full), "/sdcard%s", (*path == '/') ? path : "");
        lyra_track_meta_t meta = {0};
        if (meta_read_local(full[0] ? full : path, &meta) == ESP_OK) {
            print_fn("File: %s\r\n", path);
            if (meta.title[0])        print_fn("  Title:  %s\r\n", meta.title);
            if (meta.artist[0])       print_fn("  Artist: %s\r\n", meta.artist);
            if (meta.album[0])        print_fn("  Album:  %s\r\n", meta.album);
            if (meta.date[0])         print_fn("  Date:   %s\r\n", meta.date);
            if (meta.genre[0])        print_fn("  Genre:  %s\r\n", meta.genre);
            if (meta.track_number)    print_fn("  Track:  %lu\r\n", (unsigned long)meta.track_number);
            if (meta.musicbrainz_release_id[0])
                print_fn("  MBID:   %s\r\n", meta.musicbrainz_release_id);
            print_fn("  Cover:  %s\r\n",
                     meta.has_embedded_cover ? "embedded" :
                     meta.has_external_cover ? "cover.jpg" : "none");
        } else {
            print_fn("Cannot read: %s\r\n", path);
        }

    } else if (strncmp(subcommand, "download-all", 12) == 0) {
        print_fn("Downloading metadata for entire library (rate-limited)...\r\n");
        meta_download_library("/sdcard/Music", false, NULL);
        print_fn("Done\r\n");

    } else if (strncmp(subcommand, "download ", 9) == 0) {
        const char *path = subcommand + 9;
        while (*path == ' ') path++;
        char full[256];
        snprintf(full, sizeof(full), "/sdcard%s", (*path == '/') ? path : "");
        print_fn("Downloading metadata for %s...\r\n", path);
        esp_err_t err = meta_download_album(full[0] ? full : path, NULL);
        print_fn(err == ESP_OK ? "Done\r\n" : "Failed: %s\r\n", esp_err_to_name(err));

    } else if (strcmp(subcommand, "status") == 0) {
        uint8_t pct = meta_get_download_progress();
        print_fn("Download progress: %d%%\r\n", pct);

    } else if (strcmp(subcommand, "cancel") == 0) {
        meta_cancel_download();
        print_fn("Download cancelled\r\n");

    } else if (strncmp(subcommand, "setkey ", 7) == 0) {
        const char *rest = subcommand + 7;
        while (*rest == ' ') rest++;
        meta_service_t svc;
        const char *key;
        if (strncmp(rest, "lastfm ", 7) == 0) {
            svc = META_SERVICE_LASTFM;
            key = rest + 7;
        } else if (strncmp(rest, "acoustid ", 9) == 0) {
            svc = META_SERVICE_ACOUSTID;
            key = rest + 9;
        } else {
            print_fn("Usage: meta setkey lastfm <key> | meta setkey acoustid <key>\r\n");
            return;
        }
        while (*key == ' ') key++;
        esp_err_t err = meta_set_api_key(svc, key);
        print_fn(err == ESP_OK ? "API key saved\r\n" : "Save failed: %s\r\n", esp_err_to_name(err));

    } else {
        print_fn("Meta commands:\r\n");
        print_fn("  meta info <path>         - show local tags\r\n");
        print_fn("  meta download <path>     - download metadata for album\r\n");
        print_fn("  meta download-all        - download entire /sdcard/Music library\r\n");
        print_fn("  meta status              - download progress\r\n");
        print_fn("  meta cancel              - cancel download\r\n");
        print_fn("  meta setkey lastfm <k>  - set Last.fm API key\r\n");
        print_fn("  meta setkey acoustid <k>- set AcoustID API key\r\n");
    }
}
