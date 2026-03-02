/*
 * subsonic.c — Subsonic / Navidrome REST API client for Lyra.
 *
 * Token-based auth (v1.13.0+), JSON responses, streaming via net_audio.
 */

#include "subsonic.h"
#include "net_audio.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/md5.h"

static const char *TAG = "subsonic";

#define API_VERSION    "1.16.1"
#define CLIENT_NAME    "LyraPlayer"
#define RESPONSE_BUF   (16 * 1024)   /* 16 KB for JSON responses (PSRAM) */
#define URL_BUF_SIZE   1024

//--------------------------------------------------------------------+
// Internal state
//--------------------------------------------------------------------+

static struct {
    subsonic_state_t state;
    char server_url[256];      // "http://192.168.1.100:4533"
    char username[64];
    char password[64];
    char server_version[16];
} s_ctx;

// Track suffix cache: remembers codec suffix for recently listed tracks
#define SUFFIX_CACHE_SIZE 40
static struct {
    char id[48];
    char suffix[8];   // "flac", "mp3", etc.
} s_suffix_cache[SUFFIX_CACHE_SIZE];
static int s_suffix_cache_count = 0;

static void suffix_cache_clear(void)
{
    s_suffix_cache_count = 0;
}

static void suffix_cache_add(const char *id, const char *suffix)
{
    if (!id || !suffix || !*suffix) return;
    // Overwrite if full (ring)
    int idx = s_suffix_cache_count % SUFFIX_CACHE_SIZE;
    strncpy(s_suffix_cache[idx].id, id, sizeof(s_suffix_cache[idx].id) - 1);
    s_suffix_cache[idx].id[sizeof(s_suffix_cache[idx].id) - 1] = '\0';
    strncpy(s_suffix_cache[idx].suffix, suffix, sizeof(s_suffix_cache[idx].suffix) - 1);
    s_suffix_cache[idx].suffix[sizeof(s_suffix_cache[idx].suffix) - 1] = '\0';
    if (s_suffix_cache_count < SUFFIX_CACHE_SIZE) s_suffix_cache_count++;
}

static const char *suffix_cache_lookup(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < s_suffix_cache_count; i++) {
        if (strcmp(s_suffix_cache[i].id, id) == 0) {
            return s_suffix_cache[i].suffix;
        }
    }
    return NULL;
}

//--------------------------------------------------------------------+
// Playlist / queue
//--------------------------------------------------------------------+

#define PLAYLIST_MAX 50

typedef struct {
    char id[48];
    char title[96];
    char suffix[8];    // "flac", "mp3", etc.
    int  duration;     // seconds
} playlist_entry_t;

static struct {
    playlist_entry_t tracks[PLAYLIST_MAX];
    int  count;
    int  current;            // index of playing track (-1 = none)
    char album_name[128];
    char album_artist[128];
    bool active;             // auto-advance enabled
    net_audio_state_t last_state;
} s_playlist;

static esp_timer_handle_t s_advance_timer;

// Forward declarations
static void generate_salt(char *buf, size_t len);
static void md5_hex(const char *input, size_t input_len, char out_hex[33]);

static void play_track_at_index(int idx, subsonic_print_fn_t print)
{
    if (idx < 0 || idx >= s_playlist.count) {
        if (print) print("End of playlist.\r\n");
        s_playlist.active = false;
        s_playlist.current = -1;
        return;
    }

    playlist_entry_t *t = &s_playlist.tracks[idx];
    s_playlist.current = idx;

    const char *codec_hint = t->suffix[0] ? t->suffix : "flac";

    // Build stream URL (pure string ops — no HTTP)
    char salt[13];
    generate_salt(salt, sizeof(salt));

    char token_input[128];
    int n = snprintf(token_input, sizeof(token_input), "%s%s", s_ctx.password, salt);
    char token[33];
    md5_hex(token_input, (size_t)n, token);

    char url[URL_BUF_SIZE];
    snprintf(url, sizeof(url),
        "%s/rest/stream.view?id=%s&u=%s&t=%s&s=%s&v=%s&c=%s",
        s_ctx.server_url, t->id,
        s_ctx.username, token, salt,
        API_VERSION, CLIENT_NAME);

    if (print) {
        print("[%d/%d] %s [%s]", idx + 1, s_playlist.count, t->title, codec_hint);
        if (t->duration > 0) print("  %d:%02d", t->duration / 60, t->duration % 60);
        print("\r\n");
    }

    ESP_LOGI(TAG, "Play [%d/%d] %s [%s]", idx + 1, s_playlist.count, t->id, codec_hint);
    net_audio_cmd_start(url, codec_hint, NULL);
    s_playlist.last_state = NET_AUDIO_CONNECTING;
}

// Timer callback: detect track end and auto-advance
static void advance_timer_cb(void *arg)
{
    if (!s_playlist.active || s_playlist.current < 0) return;

    net_audio_state_t cur = net_audio_get_state();

    if ((s_playlist.last_state == NET_AUDIO_PLAYING ||
         s_playlist.last_state == NET_AUDIO_BUFFERING) &&
        cur == NET_AUDIO_IDLE) {
        // Track finished — advance
        int next = s_playlist.current + 1;
        if (next < s_playlist.count) {
            ESP_LOGI(TAG, "Auto-advance: track %d/%d", next + 1, s_playlist.count);
            play_track_at_index(next, NULL);
        } else {
            ESP_LOGI(TAG, "Playlist complete");
            s_playlist.active = false;
            s_playlist.current = -1;
        }
    }

    s_playlist.last_state = cur;
}

//--------------------------------------------------------------------+
// Helpers: salt & MD5 token
//--------------------------------------------------------------------+

static void generate_salt(char *buf, size_t len)
{
    // Generate random hex string (at least 6 chars, we use 12)
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len - 1; i++) {
        buf[i] = hex[esp_random() % 16];
    }
    buf[len - 1] = '\0';
}

static void md5_hex(const char *input, size_t input_len, char out_hex[33])
{
    uint8_t hash[16];
    mbedtls_md5((const unsigned char *)input, input_len, hash);
    for (int i = 0; i < 16; i++) {
        sprintf(out_hex + i * 2, "%02x", hash[i]);
    }
    out_hex[32] = '\0';
}

// Build full API URL with auth parameters.
// endpoint: "ping" (without .view suffix — we add it)
// extra_params: "&type=newest&size=20" or "" or NULL
static int build_api_url(const char *endpoint, const char *extra_params,
                          char *buf, size_t buf_len)
{
    char salt[13];
    generate_salt(salt, sizeof(salt));

    // token = MD5(password + salt)
    char token_input[128];
    int n = snprintf(token_input, sizeof(token_input), "%s%s", s_ctx.password, salt);
    char token[33];
    md5_hex(token_input, (size_t)n, token);

    return snprintf(buf, buf_len,
        "%s/rest/%s.view?u=%s&t=%s&s=%s&v=%s&c=%s&f=json%s%s",
        s_ctx.server_url, endpoint,
        s_ctx.username, token, salt,
        API_VERSION, CLIENT_NAME,
        (extra_params && extra_params[0]) ? "&" : "",
        (extra_params && extra_params[0]) ? extra_params : "");
}

//--------------------------------------------------------------------+
// HTTP GET → cJSON*
//--------------------------------------------------------------------+

// HTTP event handler to accumulate response body
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_ctx_t *ctx = (http_resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        if (ctx->len + evt->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

// Perform HTTP GET, parse JSON response. Caller must cJSON_Delete() result.
// Returns NULL on error (prints error via print_fn if provided).
static cJSON *api_get_json(const char *url, subsonic_print_fn_t print)
{
    char *resp_buf = heap_caps_malloc(RESPONSE_BUF, MALLOC_CAP_SPIRAM);
    if (!resp_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%d bytes)", RESPONSE_BUF);
        if (print) print("Error: memory allocation failed\r\n");
        return NULL;
    }
    resp_buf[0] = '\0';

    http_resp_ctx_t resp_ctx = {
        .buf = resp_buf,
        .len = 0,
        .cap = RESPONSE_BUF,
    };

    esp_http_client_config_t cfg = {
        .url                   = url,
        .timeout_ms            = 15000,
        .buffer_size           = 4096,
        .buffer_size_tx        = 512,
        .max_redirection_count = 5,
        .user_agent            = "LyraPlayer/1.0",
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .event_handler         = http_event_handler,
        .user_data             = &resp_ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        if (print) print("Error: HTTP client init failed\r\n");
        heap_caps_free(resp_buf);
        return NULL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        if (print) print("Error: HTTP request failed (%s)\r\n", esp_err_to_name(err));
        heap_caps_free(resp_buf);
        return NULL;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d", status);
        if (print) print("Error: HTTP %d\r\n", status);
        heap_caps_free(resp_buf);
        return NULL;
    }

    // Parse JSON
    cJSON *root = cJSON_Parse(resp_buf);
    heap_caps_free(resp_buf);  // Free response buffer immediately

    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        if (print) print("Error: JSON parse failed\r\n");
        return NULL;
    }

    return root;
}

// Check subsonic-response status. Returns the inner response object on success, NULL on error.
static cJSON *check_response(cJSON *root, subsonic_print_fn_t print)
{
    cJSON *resp = cJSON_GetObjectItem(root, "subsonic-response");
    if (!resp) {
        if (print) print("Error: invalid response format\r\n");
        return NULL;
    }

    cJSON *status = cJSON_GetObjectItem(resp, "status");
    if (!status || !cJSON_IsString(status)) {
        if (print) print("Error: no status in response\r\n");
        return NULL;
    }

    if (strcmp(status->valuestring, "ok") != 0) {
        // Error response
        cJSON *err = cJSON_GetObjectItem(resp, "error");
        if (err) {
            cJSON *msg = cJSON_GetObjectItem(err, "message");
            cJSON *code = cJSON_GetObjectItem(err, "code");
            if (print) {
                print("Server error %d: %s\r\n",
                      code ? code->valueint : -1,
                      (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
            }
        } else {
            if (print) print("Server error: %s\r\n", status->valuestring);
        }
        return NULL;
    }

    return resp;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void subsonic_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = SUBSONIC_DISCONNECTED;

    memset(&s_playlist, 0, sizeof(s_playlist));
    s_playlist.current = -1;

    // Create auto-advance timer (1 second periodic)
    esp_timer_create_args_t timer_args = {
        .callback = advance_timer_cb,
        .name = "sub_adv",
    };
    esp_timer_create(&timer_args, &s_advance_timer);
    esp_timer_start_periodic(s_advance_timer, 1000000);  // 1s

    ESP_LOGI(TAG, "Subsonic client initialized");
}

subsonic_state_t subsonic_get_state(void)
{
    return s_ctx.state;
}

const char *subsonic_get_server_url(void)
{
    return s_ctx.server_url;
}

const char *subsonic_get_username(void)
{
    return s_ctx.username;
}

esp_err_t subsonic_connect(const char *server_url, const char *user,
                            const char *password, subsonic_print_fn_t print)
{
    // Auto-prepend http:// if no scheme given
    if (strncmp(server_url, "http://", 7) != 0 &&
        strncmp(server_url, "https://", 8) != 0) {
        snprintf(s_ctx.server_url, sizeof(s_ctx.server_url), "http://%s", server_url);
    } else {
        strncpy(s_ctx.server_url, server_url, sizeof(s_ctx.server_url) - 1);
    }
    strncpy(s_ctx.username, user, sizeof(s_ctx.username) - 1);
    strncpy(s_ctx.password, password, sizeof(s_ctx.password) - 1);

    // Remove trailing slash from URL
    size_t url_len = strlen(s_ctx.server_url);
    if (url_len > 0 && s_ctx.server_url[url_len - 1] == '/') {
        s_ctx.server_url[url_len - 1] = '\0';
    }

    s_ctx.state = SUBSONIC_CONNECTING;
    if (print) print("Connecting to %s as %s...\r\n", s_ctx.server_url, s_ctx.username);

    // Build ping URL
    char url[URL_BUF_SIZE];
    build_api_url("ping", NULL, url, sizeof(url));

    ESP_LOGI(TAG, "Ping: %s", url);

    cJSON *root = api_get_json(url, print);
    if (!root) {
        s_ctx.state = SUBSONIC_ERROR;
        return ESP_FAIL;
    }

    cJSON *resp = check_response(root, print);
    if (!resp) {
        s_ctx.state = SUBSONIC_ERROR;
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Extract server version
    cJSON *ver = cJSON_GetObjectItem(resp, "version");
    if (ver && cJSON_IsString(ver)) {
        strncpy(s_ctx.server_version, ver->valuestring, sizeof(s_ctx.server_version) - 1);
    }

    s_ctx.state = SUBSONIC_CONNECTED;
    ESP_LOGI(TAG, "Connected to %s (v%s)", s_ctx.server_url, s_ctx.server_version);
    if (print) print("Connected! Server version: %s\r\n", s_ctx.server_version);

    cJSON_Delete(root);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// CDC command handlers
//--------------------------------------------------------------------+

static void cmd_connect(const char *args, subsonic_print_fn_t print)
{
    // Parse: <url> <user> <pass>
    char url_buf[256], user_buf[64], pass_buf[64];
    url_buf[0] = user_buf[0] = pass_buf[0] = '\0';

    // Extract URL (first arg)
    const char *p = args;
    while (*p == ' ') p++;
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t len = (size_t)(p - start);
    if (len == 0 || len >= sizeof(url_buf)) {
        print("Usage: subsonic connect <url> <user> <pass>\r\n");
        return;
    }
    memcpy(url_buf, start, len);
    url_buf[len] = '\0';

    // Extract username
    while (*p == ' ') p++;
    start = p;
    while (*p && *p != ' ') p++;
    len = (size_t)(p - start);
    if (len == 0 || len >= sizeof(user_buf)) {
        print("Usage: subsonic connect <url> <user> <pass>\r\n");
        return;
    }
    memcpy(user_buf, start, len);
    user_buf[len] = '\0';

    // Extract password (rest of line)
    while (*p == ' ') p++;
    if (!*p) {
        print("Usage: subsonic connect <url> <user> <pass>\r\n");
        return;
    }
    strncpy(pass_buf, p, sizeof(pass_buf) - 1);
    // Trim trailing whitespace
    len = strlen(pass_buf);
    while (len > 0 && (pass_buf[len-1] == ' ' || pass_buf[len-1] == '\r' || pass_buf[len-1] == '\n')) {
        pass_buf[--len] = '\0';
    }

    subsonic_connect(url_buf, user_buf, pass_buf, print);
}

static void cmd_ping(subsonic_print_fn_t print)
{
    if (s_ctx.state == SUBSONIC_DISCONNECTED) {
        print("Not connected. Use: subsonic connect <url> <user> <pass>\r\n");
        return;
    }

    char url[URL_BUF_SIZE];
    build_api_url("ping", NULL, url, sizeof(url));

    cJSON *root = api_get_json(url, print);
    if (!root) return;

    cJSON *resp = check_response(root, print);
    if (resp) {
        print("Pong! Server OK (v%s)\r\n", s_ctx.server_version);
    }
    cJSON_Delete(root);
}

static void cmd_albums(const char *args, subsonic_print_fn_t print)
{
    if (s_ctx.state != SUBSONIC_CONNECTED) {
        print("Not connected. Use: subsonic connect <url> <user> <pass>\r\n");
        return;
    }

    // Default type: "newest"
    const char *type = "newest";
    if (args && *args) {
        type = args;
    }

    char params[128];
    snprintf(params, sizeof(params), "type=%s&size=20&offset=0", type);

    char url[URL_BUF_SIZE];
    build_api_url("getAlbumList2", params, url, sizeof(url));

    print("Fetching albums (%s)...\r\n", type);

    cJSON *root = api_get_json(url, print);
    if (!root) return;

    cJSON *resp = check_response(root, print);
    if (!resp) {
        cJSON_Delete(root);
        return;
    }

    cJSON *album_list = cJSON_GetObjectItem(resp, "albumList2");
    cJSON *albums = album_list ? cJSON_GetObjectItem(album_list, "album") : NULL;

    if (!albums || !cJSON_IsArray(albums)) {
        print("No albums found.\r\n");
        cJSON_Delete(root);
        return;
    }

    int count = cJSON_GetArraySize(albums);
    print("=== Albums (%d) ===\r\n", count);

    for (int i = 0; i < count; i++) {
        cJSON *a = cJSON_GetArrayItem(albums, i);
        const char *id     = cJSON_GetStringValue(cJSON_GetObjectItem(a, "id"));
        const char *name   = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
        const char *artist = cJSON_GetStringValue(cJSON_GetObjectItem(a, "artist"));
        cJSON *year_j      = cJSON_GetObjectItem(a, "year");
        cJSON *songs_j     = cJSON_GetObjectItem(a, "songCount");

        int year  = (year_j && cJSON_IsNumber(year_j)) ? year_j->valueint : 0;
        int songs = (songs_j && cJSON_IsNumber(songs_j)) ? songs_j->valueint : 0;

        print("  [%s]\r\n", id ? id : "?");
        print("    %s - %s", artist ? artist : "?", name ? name : "?");
        if (year > 0) print(" (%d)", year);
        if (songs > 0) print(" [%d tracks]", songs);
        print("\r\n");
    }

    cJSON_Delete(root);
}

static void cmd_album(const char *album_id, subsonic_print_fn_t print)
{
    if (s_ctx.state != SUBSONIC_CONNECTED) {
        print("Not connected.\r\n");
        return;
    }

    if (!album_id || !*album_id) {
        print("Usage: subsonic album <album_id>\r\n");
        return;
    }

    char params[128];
    snprintf(params, sizeof(params), "id=%s", album_id);

    char url[URL_BUF_SIZE];
    build_api_url("getAlbum", params, url, sizeof(url));

    print("Fetching album...\r\n");

    cJSON *root = api_get_json(url, print);
    if (!root) return;

    cJSON *resp = check_response(root, print);
    if (!resp) {
        cJSON_Delete(root);
        return;
    }

    cJSON *album = cJSON_GetObjectItem(resp, "album");
    if (!album) {
        print("Album not found.\r\n");
        cJSON_Delete(root);
        return;
    }

    const char *album_name = cJSON_GetStringValue(cJSON_GetObjectItem(album, "name"));
    const char *artist     = cJSON_GetStringValue(cJSON_GetObjectItem(album, "artist"));
    print("=== %s - %s ===\r\n", artist ? artist : "?", album_name ? album_name : "?");

    cJSON *songs = cJSON_GetObjectItem(album, "song");
    if (!songs || !cJSON_IsArray(songs)) {
        print("No tracks.\r\n");
        cJSON_Delete(root);
        return;
    }

    int count = cJSON_GetArraySize(songs);
    suffix_cache_clear();
    for (int i = 0; i < count; i++) {
        cJSON *s = cJSON_GetArrayItem(songs, i);
        const char *id     = cJSON_GetStringValue(cJSON_GetObjectItem(s, "id"));
        const char *title  = cJSON_GetStringValue(cJSON_GetObjectItem(s, "title"));
        cJSON *track_j     = cJSON_GetObjectItem(s, "track");
        cJSON *dur_j       = cJSON_GetObjectItem(s, "duration");
        cJSON *br_j        = cJSON_GetObjectItem(s, "bitRate");
        const char *suffix = cJSON_GetStringValue(cJSON_GetObjectItem(s, "suffix"));

        int track_num = (track_j && cJSON_IsNumber(track_j)) ? track_j->valueint : 0;
        int dur_s     = (dur_j && cJSON_IsNumber(dur_j)) ? dur_j->valueint : 0;
        int bitrate   = (br_j && cJSON_IsNumber(br_j)) ? br_j->valueint : 0;

        // Cache suffix for codec hint when playing
        suffix_cache_add(id, suffix);

        print("  %2d. [%s] %s",
              track_num, id ? id : "?", title ? title : "?");
        if (dur_s > 0) print("  %d:%02d", dur_s / 60, dur_s % 60);
        if (suffix) print("  %s", suffix);
        if (bitrate > 0) print("  %dkbps", bitrate);
        print("\r\n");
    }

    cJSON_Delete(root);
}

static void cmd_search(const char *query, subsonic_print_fn_t print)
{
    if (s_ctx.state != SUBSONIC_CONNECTED) {
        print("Not connected.\r\n");
        return;
    }

    if (!query || !*query) {
        print("Usage: subsonic search <query>\r\n");
        return;
    }

    // URL-encode spaces as + (basic encoding, sufficient for most queries)
    char encoded[128];
    size_t ei = 0;
    for (size_t i = 0; query[i] && ei < sizeof(encoded) - 4; i++) {
        if (query[i] == ' ') {
            encoded[ei++] = '+';
        } else if (isalnum((unsigned char)query[i]) || query[i] == '-' ||
                   query[i] == '_' || query[i] == '.' || query[i] == '~') {
            encoded[ei++] = query[i];
        } else {
            // Percent-encode
            snprintf(encoded + ei, 4, "%%%02X", (unsigned char)query[i]);
            ei += 3;
        }
    }
    encoded[ei] = '\0';

    char params[256];
    snprintf(params, sizeof(params),
             "query=%s&albumCount=10&songCount=10&artistCount=5", encoded);

    char url[URL_BUF_SIZE];
    build_api_url("search3", params, url, sizeof(url));

    print("Searching '%s'...\r\n", query);

    cJSON *root = api_get_json(url, print);
    if (!root) return;

    cJSON *resp = check_response(root, print);
    if (!resp) {
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItem(resp, "searchResult3");
    if (!result) {
        print("No results.\r\n");
        cJSON_Delete(root);
        return;
    }

    // Artists
    cJSON *artists = cJSON_GetObjectItem(result, "artist");
    if (artists && cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
        print("--- Artists ---\r\n");
        int n = cJSON_GetArraySize(artists);
        for (int i = 0; i < n; i++) {
            cJSON *a = cJSON_GetArrayItem(artists, i);
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
            cJSON *ac_j = cJSON_GetObjectItem(a, "albumCount");
            int ac = (ac_j && cJSON_IsNumber(ac_j)) ? ac_j->valueint : 0;
            print("  %s (%d albums)\r\n", name ? name : "?", ac);
        }
    }

    // Albums
    cJSON *albums = cJSON_GetObjectItem(result, "album");
    if (albums && cJSON_IsArray(albums) && cJSON_GetArraySize(albums) > 0) {
        print("--- Albums ---\r\n");
        int n = cJSON_GetArraySize(albums);
        for (int i = 0; i < n; i++) {
            cJSON *a = cJSON_GetArrayItem(albums, i);
            const char *id     = cJSON_GetStringValue(cJSON_GetObjectItem(a, "id"));
            const char *name   = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
            const char *artist = cJSON_GetStringValue(cJSON_GetObjectItem(a, "artist"));
            print("  [%s] %s - %s\r\n", id ? id : "?",
                  artist ? artist : "?", name ? name : "?");
        }
    }

    // Songs
    cJSON *songs = cJSON_GetObjectItem(result, "song");
    if (songs && cJSON_IsArray(songs) && cJSON_GetArraySize(songs) > 0) {
        print("--- Songs ---\r\n");
        suffix_cache_clear();
        int n = cJSON_GetArraySize(songs);
        for (int i = 0; i < n; i++) {
            cJSON *s = cJSON_GetArrayItem(songs, i);
            const char *id     = cJSON_GetStringValue(cJSON_GetObjectItem(s, "id"));
            const char *title  = cJSON_GetStringValue(cJSON_GetObjectItem(s, "title"));
            const char *artist = cJSON_GetStringValue(cJSON_GetObjectItem(s, "artist"));
            const char *album  = cJSON_GetStringValue(cJSON_GetObjectItem(s, "album"));
            cJSON *dur_j = cJSON_GetObjectItem(s, "duration");
            int dur = (dur_j && cJSON_IsNumber(dur_j)) ? dur_j->valueint : 0;
            const char *suffix = cJSON_GetStringValue(cJSON_GetObjectItem(s, "suffix"));

            // Cache suffix for codec hint when playing
            suffix_cache_add(id, suffix);

            print("  [%s] %s - %s",
                  id ? id : "?", artist ? artist : "?", title ? title : "?");
            if (album) print(" (%s)", album);
            if (dur > 0) print("  %d:%02d", dur / 60, dur % 60);
            if (suffix) print("  %s", suffix);
            print("\r\n");
        }
    }

    cJSON_Delete(root);
}

// Populate playlist from a cJSON album object (must have "song" array).
// Returns number of tracks added.
static int populate_playlist(cJSON *album)
{
    const char *name   = cJSON_GetStringValue(cJSON_GetObjectItem(album, "name"));
    const char *artist = cJSON_GetStringValue(cJSON_GetObjectItem(album, "artist"));

    strncpy(s_playlist.album_name, name ? name : "?", sizeof(s_playlist.album_name) - 1);
    strncpy(s_playlist.album_artist, artist ? artist : "?", sizeof(s_playlist.album_artist) - 1);

    cJSON *songs = cJSON_GetObjectItem(album, "song");
    if (!songs || !cJSON_IsArray(songs)) return 0;

    int count = cJSON_GetArraySize(songs);
    if (count > PLAYLIST_MAX) count = PLAYLIST_MAX;

    s_playlist.count = 0;
    suffix_cache_clear();

    for (int i = 0; i < count; i++) {
        cJSON *s = cJSON_GetArrayItem(songs, i);
        playlist_entry_t *e = &s_playlist.tracks[i];

        const char *id     = cJSON_GetStringValue(cJSON_GetObjectItem(s, "id"));
        const char *title  = cJSON_GetStringValue(cJSON_GetObjectItem(s, "title"));
        const char *suffix = cJSON_GetStringValue(cJSON_GetObjectItem(s, "suffix"));
        cJSON *dur_j       = cJSON_GetObjectItem(s, "duration");

        if (id)     strncpy(e->id, id, sizeof(e->id) - 1);
        if (title)  strncpy(e->title, title, sizeof(e->title) - 1);
        if (suffix) strncpy(e->suffix, suffix, sizeof(e->suffix) - 1);
        e->duration = (dur_j && cJSON_IsNumber(dur_j)) ? dur_j->valueint : 0;

        suffix_cache_add(id, suffix);
        s_playlist.count++;
    }

    return s_playlist.count;
}

static void cmd_play(const char *args, subsonic_print_fn_t print)
{
    if (s_ctx.state != SUBSONIC_CONNECTED) {
        print("Not connected.\r\n");
        return;
    }

    if (!args || !*args) {
        print("Usage: subsonic play <album_or_track_id>\r\n");
        return;
    }

    // Extract ID (first token)
    char play_id[64];
    const char *p = args;
    while (*p == ' ') p++;
    const char *start = p;
    while (*p && *p != ' ') p++;
    size_t len = (size_t)(p - start);
    if (len >= sizeof(play_id)) len = sizeof(play_id) - 1;
    memcpy(play_id, start, len);
    play_id[len] = '\0';

    // --- Try as album first (getAlbum) ---
    char params[128];
    snprintf(params, sizeof(params), "id=%s", play_id);

    char api_url[URL_BUF_SIZE];
    build_api_url("getAlbum", params, api_url, sizeof(api_url));

    cJSON *root = api_get_json(api_url, NULL);  // silent
    if (root) {
        cJSON *resp = check_response(root, NULL);
        if (resp) {
            cJSON *album = cJSON_GetObjectItem(resp, "album");
            cJSON *songs = album ? cJSON_GetObjectItem(album, "song") : NULL;
            if (songs && cJSON_IsArray(songs) && cJSON_GetArraySize(songs) > 0) {
                // It's an album — populate playlist and play
                int n = populate_playlist(album);
                cJSON_Delete(root);

                print("=== %s - %s (%d tracks) ===\r\n",
                      s_playlist.album_artist, s_playlist.album_name, n);
                s_playlist.active = true;
                s_playlist.last_state = NET_AUDIO_IDLE;
                play_track_at_index(0, print);
                return;
            }
        }
        cJSON_Delete(root);
    }

    // --- Not an album — play as single track ---

    // Deactivate playlist auto-advance for single tracks
    s_playlist.active = false;
    s_playlist.current = -1;
    s_playlist.count = 0;

    // 1) Try suffix cache
    const char *codec_hint = suffix_cache_lookup(play_id);

    // 2) If not cached, ask the server via getSong
    char fetched_suffix[16] = {0};
    if (!codec_hint) {
        snprintf(params, sizeof(params), "id=%s", play_id);
        build_api_url("getSong", params, api_url, sizeof(api_url));

        root = api_get_json(api_url, NULL);
        if (root) {
            cJSON *resp2 = check_response(root, NULL);
            if (resp2) {
                cJSON *song = cJSON_GetObjectItem(resp2, "song");
                if (song) {
                    const char *suf = cJSON_GetStringValue(cJSON_GetObjectItem(song, "suffix"));
                    if (suf && *suf) {
                        strncpy(fetched_suffix, suf, sizeof(fetched_suffix) - 1);
                        codec_hint = fetched_suffix;
                        suffix_cache_add(play_id, suf);
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    // 3) Fallback: assume flac
    if (!codec_hint) {
        codec_hint = "flac";
        ESP_LOGW(TAG, "Could not determine codec, defaulting to flac");
    }

    ESP_LOGI(TAG, "Codec: %s", codec_hint);

    // Build stream URL
    char salt[13];
    generate_salt(salt, sizeof(salt));

    char token_input[128];
    int n = snprintf(token_input, sizeof(token_input), "%s%s", s_ctx.password, salt);
    char token[33];
    md5_hex(token_input, (size_t)n, token);

    char url[URL_BUF_SIZE];
    snprintf(url, sizeof(url),
        "%s/rest/stream.view?id=%s&u=%s&t=%s&s=%s&v=%s&c=%s",
        s_ctx.server_url, play_id,
        s_ctx.username, token, salt,
        API_VERSION, CLIENT_NAME);

    ESP_LOGI(TAG, "Streaming: %s", url);
    print("Streaming: %s [%s]\r\n", play_id, codec_hint);

    esp_err_t err = net_audio_cmd_start(url, codec_hint, NULL);
    if (err == ESP_OK) {
        print("Playback started.\r\n");
    } else {
        print("Error: net_audio queue full or not ready\r\n");
    }
}

static void cmd_next(subsonic_print_fn_t print)
{
    if (!s_playlist.active || s_playlist.current < 0) {
        print("No playlist active.\r\n");
        return;
    }

    int next = s_playlist.current + 1;
    if (next >= s_playlist.count) {
        print("Already at last track.\r\n");
        return;
    }

    s_playlist.last_state = NET_AUDIO_IDLE;  // prevent timer double-advance
    net_audio_cmd_stop();
    play_track_at_index(next, print);
}

static void cmd_prev(subsonic_print_fn_t print)
{
    if (!s_playlist.active || s_playlist.current < 0) {
        print("No playlist active.\r\n");
        return;
    }

    int prev = s_playlist.current - 1;
    if (prev < 0) prev = 0;  // restart first track

    s_playlist.last_state = NET_AUDIO_IDLE;  // prevent timer double-advance
    net_audio_cmd_stop();
    play_track_at_index(prev, print);
}

static void cmd_status(subsonic_print_fn_t print)
{
    const char *state_names[] = {
        "disconnected", "connecting", "connected", "error"
    };
    print("Subsonic: %s\r\n", state_names[s_ctx.state]);

    if (s_ctx.state == SUBSONIC_CONNECTED) {
        print("  Server:  %s (v%s)\r\n", s_ctx.server_url, s_ctx.server_version);
        print("  User:    %s\r\n", s_ctx.username);
    }

    // Playlist info
    if (s_playlist.active && s_playlist.current >= 0) {
        print("  Playlist: %s - %s\r\n", s_playlist.album_artist, s_playlist.album_name);
        print("  Track:    %d/%d  %s\r\n",
              s_playlist.current + 1, s_playlist.count,
              s_playlist.tracks[s_playlist.current].title);
    }

    // Net audio state
    if (net_audio_is_active()) {
        net_audio_info_t info = net_audio_get_info();
        print("  Stream:  %s %luHz %d-bit\r\n",
              info.codec, (unsigned long)info.sample_rate, info.bits_per_sample);
        print("  Elapsed: %lu:%02lu\r\n",
              (unsigned long)(info.elapsed_ms / 60000),
              (unsigned long)((info.elapsed_ms / 1000) % 60));
    }
}

//--------------------------------------------------------------------+
// CDC dispatcher
//--------------------------------------------------------------------+

void subsonic_handle_cdc_command(const char *sub, subsonic_print_fn_t print)
{
    if (!sub || !*sub) {
        print("Commands:\r\n");
        print("  subsonic connect <url> <user> <pass>\r\n");
        print("  subsonic ping\r\n");
        print("  subsonic albums [newest|random|frequent|recent|alphabeticalByName]\r\n");
        print("  subsonic album <id>\r\n");
        print("  subsonic search <query>\r\n");
        print("  subsonic play <album_or_track_id>\r\n");
        print("  subsonic next / prev\r\n");
        print("  subsonic stop\r\n");
        print("  subsonic status\r\n");
        return;
    }

    if (strncmp(sub, "connect ", 8) == 0) {
        cmd_connect(sub + 8, print);
    } else if (strcmp(sub, "ping") == 0) {
        cmd_ping(print);
    } else if (strncmp(sub, "albums", 6) == 0) {
        const char *type = sub + 6;
        while (*type == ' ') type++;
        cmd_albums(*type ? type : NULL, print);
    } else if (strncmp(sub, "album ", 6) == 0) {
        const char *id = sub + 6;
        while (*id == ' ') id++;
        cmd_album(id, print);
    } else if (strncmp(sub, "search ", 7) == 0) {
        const char *q = sub + 7;
        while (*q == ' ') q++;
        cmd_search(q, print);
    } else if (strncmp(sub, "play ", 5) == 0) {
        const char *id = sub + 5;
        while (*id == ' ') id++;
        cmd_play(id, print);
    } else if (strcmp(sub, "next") == 0) {
        cmd_next(print);
    } else if (strcmp(sub, "prev") == 0) {
        cmd_prev(print);
    } else if (strcmp(sub, "stop") == 0) {
        s_playlist.active = false;
        s_playlist.current = -1;
        s_playlist.count = 0;
        net_audio_cmd_stop();
        print("Stopped.\r\n");
    } else if (strcmp(sub, "status") == 0) {
        cmd_status(print);
    } else {
        print("Unknown subcommand: %s\r\n", sub);
        print("Type 'subsonic' for help\r\n");
    }
}
