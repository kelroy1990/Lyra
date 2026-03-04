#include "lastfm.h"
#include "settings_store.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/md5.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "lastfm";

#define LASTFM_API_URL  "https://ws.audioscrobbler.com/2.0/"
#define LASTFM_QUEUE_MAX 32

//--------------------------------------------------------------------+
// Scrobble queue entry (stored in NVS)
//--------------------------------------------------------------------+

typedef struct {
    char artist[128];
    char track[128];
    char album[128];
    uint32_t duration_s;
    uint32_t timestamp;
} lastfm_scrobble_entry_t;

//--------------------------------------------------------------------+
// Module state
//--------------------------------------------------------------------+

static struct {
    settings_lastfm_t cfg;
    bool authenticated;

    // Circular queue indices (persisted in NVS)
    uint8_t q_head;  // next to send
    uint8_t q_tail;  // next to write
    uint8_t q_count;
} s_lfm;

//--------------------------------------------------------------------+
// MD5 helpers
//--------------------------------------------------------------------+

static void md5_hex(const char *input, size_t len, char out[33])
{
    unsigned char hash[16];
    mbedtls_md5((const unsigned char *)input, len, hash);
    for (int i = 0; i < 16; i++) {
        sprintf(out + i * 2, "%02x", hash[i]);
    }
    out[32] = '\0';
}

// Build api_sig: sort params alphabetically, concatenate, append secret, MD5
// params = array of key-value pairs: {"api_key", "xxx", "method", "yyy", ...}
// num_pairs = number of key-value pairs
static void build_api_sig(const char **params, int num_pairs, char sig[33])
{
    // Simple bubble sort of param pairs by key
    // Copy pointers for sorting
    const char *sorted[40];  // max 20 pairs
    if (num_pairs > 20) num_pairs = 20;
    for (int i = 0; i < num_pairs * 2; i++) {
        sorted[i] = params[i];
    }
    for (int i = 0; i < num_pairs - 1; i++) {
        for (int j = i + 1; j < num_pairs; j++) {
            if (strcmp(sorted[i * 2], sorted[j * 2]) > 0) {
                const char *tk = sorted[i * 2];
                const char *tv = sorted[i * 2 + 1];
                sorted[i * 2] = sorted[j * 2];
                sorted[i * 2 + 1] = sorted[j * 2 + 1];
                sorted[j * 2] = tk;
                sorted[j * 2 + 1] = tv;
            }
        }
    }

    // Concatenate sorted key=value pairs + secret
    char buf[1024];
    int pos = 0;
    for (int i = 0; i < num_pairs; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s",
                        sorted[i * 2], sorted[i * 2 + 1]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", s_lfm.cfg.shared_secret);

    md5_hex(buf, pos, sig);
}

//--------------------------------------------------------------------+
// HTTP helpers
//--------------------------------------------------------------------+

typedef struct {
    char *buf;
    int   len;
    int   capacity;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (!resp) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len < resp->capacity) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

// URL-encode a string (minimal: spaces, &, =, +, special chars)
static int url_encode(const char *src, char *dst, int dst_size)
{
    int pos = 0;
    while (*src && pos < dst_size - 4) {
        char c = *src++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else {
            pos += snprintf(dst + pos, dst_size - pos, "%%%02X", (unsigned char)c);
        }
    }
    dst[pos] = '\0';
    return pos;
}

// POST to Last.fm API, return parsed JSON (caller must cJSON_Delete)
static cJSON *api_post(const char **params, int num_pairs)
{
    // Build POST body
    char body[2048];
    int pos = 0;
    for (int i = 0; i < num_pairs; i++) {
        if (i > 0) body[pos++] = '&';
        char encoded[256];
        url_encode(params[i * 2 + 1], encoded, sizeof(encoded));
        pos += snprintf(body + pos, sizeof(body) - pos, "%s=%s",
                        params[i * 2], encoded);
    }

    // Add api_sig
    char sig[33];
    build_api_sig(params, num_pairs, sig);
    pos += snprintf(body + pos, sizeof(body) - pos, "&api_sig=%s&format=json", sig);

    // HTTP POST
    static char resp_buf[4096];
    http_resp_t resp = { .buf = resp_buf, .len = 0, .capacity = sizeof(resp_buf) };

    esp_http_client_config_t config = {
        .url = LASTFM_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, pos);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "API POST failed: err=%s status=%d", esp_err_to_name(err), status);
        return NULL;
    }

    return cJSON_Parse(resp_buf);
}

//--------------------------------------------------------------------+
// NVS queue management
//--------------------------------------------------------------------+

static void queue_save_indices(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(SETTINGS_NVS_PARTITION, "lfm_q",
                                 NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "head", s_lfm.q_head);
        nvs_set_u8(h, "tail", s_lfm.q_tail);
        nvs_set_u8(h, "count", s_lfm.q_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void queue_load_indices(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(SETTINGS_NVS_PARTITION, "lfm_q",
                                 NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "head", &s_lfm.q_head);
        nvs_get_u8(h, "tail", &s_lfm.q_tail);
        nvs_get_u8(h, "count", &s_lfm.q_count);
        nvs_close(h);
    }
}

static void queue_push(const lastfm_scrobble_entry_t *entry)
{
    char ns[8];
    snprintf(ns, sizeof(ns), "lfm_q");

    char key[8];
    snprintf(key, sizeof(key), "q_%d", s_lfm.q_tail);

    nvs_handle_t h;
    if (nvs_open_from_partition(SETTINGS_NVS_PARTITION, ns,
                                 NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, entry, sizeof(*entry));
        nvs_commit(h);
        nvs_close(h);
    }

    s_lfm.q_tail = (s_lfm.q_tail + 1) % LASTFM_QUEUE_MAX;
    if (s_lfm.q_count < LASTFM_QUEUE_MAX) {
        s_lfm.q_count++;
    } else {
        // Queue full — drop oldest
        s_lfm.q_head = (s_lfm.q_head + 1) % LASTFM_QUEUE_MAX;
    }
    queue_save_indices();
}

static bool queue_peek(lastfm_scrobble_entry_t *entry)
{
    if (s_lfm.q_count == 0) return false;

    char key[8];
    snprintf(key, sizeof(key), "q_%d", s_lfm.q_head);

    nvs_handle_t h;
    if (nvs_open_from_partition(SETTINGS_NVS_PARTITION, "lfm_q",
                                 NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = sizeof(*entry);
    esp_err_t err = nvs_get_blob(h, key, entry, &len);
    nvs_close(h);
    return err == ESP_OK;
}

static void queue_pop(void)
{
    if (s_lfm.q_count == 0) return;
    s_lfm.q_head = (s_lfm.q_head + 1) % LASTFM_QUEUE_MAX;
    s_lfm.q_count--;
    queue_save_indices();
}

//--------------------------------------------------------------------+
// Send a single scrobble to Last.fm
//--------------------------------------------------------------------+

static bool send_scrobble(const lastfm_scrobble_entry_t *entry)
{
    char ts[16];
    snprintf(ts, sizeof(ts), "%lu", (unsigned long)entry->timestamp);
    char dur[16];
    snprintf(dur, sizeof(dur), "%lu", (unsigned long)entry->duration_s);

    const char *params[] = {
        "method",     "track.scrobble",
        "api_key",    s_lfm.cfg.api_key,
        "sk",         s_lfm.cfg.session_key,
        "artist[0]",  entry->artist,
        "track[0]",   entry->track,
        "album[0]",   entry->album,
        "duration[0]", dur,
        "timestamp[0]", ts,
    };

    cJSON *root = api_post(params, 8);
    if (!root) return false;

    cJSON *scrobbles = cJSON_GetObjectItem(root, "scrobbles");
    bool ok = (scrobbles != NULL);
    if (!ok) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        if (err) ESP_LOGE(TAG, "Scrobble error: %d", err->valueint);
    }
    cJSON_Delete(root);
    return ok;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

esp_err_t lastfm_init(void)
{
    memset(&s_lfm, 0, sizeof(s_lfm));
    settings_load_lastfm(&s_lfm.cfg);
    queue_load_indices();

    s_lfm.authenticated = (s_lfm.cfg.session_key[0] != '\0');

    ESP_LOGI(TAG, "Last.fm init: user=%s authenticated=%d pending=%d",
             s_lfm.cfg.username[0] ? s_lfm.cfg.username : "(none)",
             s_lfm.authenticated, s_lfm.q_count);
    return ESP_OK;
}

void lastfm_set_api_key(const char *api_key, const char *shared_secret)
{
    strncpy(s_lfm.cfg.api_key, api_key, sizeof(s_lfm.cfg.api_key) - 1);
    strncpy(s_lfm.cfg.shared_secret, shared_secret, sizeof(s_lfm.cfg.shared_secret) - 1);
    settings_save_lastfm(&s_lfm.cfg);
    ESP_LOGI(TAG, "API key set");
}

esp_err_t lastfm_authenticate(const char *username, const char *password,
                               lastfm_print_fn_t print)
{
    if (!s_lfm.cfg.api_key[0] || !s_lfm.cfg.shared_secret[0]) {
        if (print) print("[ERR] API key not set. Use: lastfm auth <key> <secret>\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    // Last.fm mobile auth: password is sent as-is (API handles hashing)
    const char *params[] = {
        "method",   "auth.getMobileSession",
        "api_key",  s_lfm.cfg.api_key,
        "username", username,
        "password", password,
    };

    if (print) print("Authenticating with Last.fm...\r\n");

    cJSON *root = api_post(params, 4);
    if (!root) {
        if (print) print("[ERR] HTTP request failed\r\n");
        return ESP_FAIL;
    }

    cJSON *session = cJSON_GetObjectItem(root, "session");
    if (!session) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (print) print("[ERR] Auth failed: %s (code %d)\r\n",
                         msg ? msg->valuestring : "unknown",
                         err ? err->valueint : -1);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *key = cJSON_GetObjectItem(session, "key");
    cJSON *name = cJSON_GetObjectItem(session, "name");

    if (key && key->valuestring) {
        strncpy(s_lfm.cfg.session_key, key->valuestring,
                sizeof(s_lfm.cfg.session_key) - 1);
    }
    if (name && name->valuestring) {
        strncpy(s_lfm.cfg.username, name->valuestring,
                sizeof(s_lfm.cfg.username) - 1);
    } else {
        strncpy(s_lfm.cfg.username, username, sizeof(s_lfm.cfg.username) - 1);
    }

    s_lfm.authenticated = true;
    settings_save_lastfm(&s_lfm.cfg);

    if (print) print("[OK] Logged in as %s\r\n", s_lfm.cfg.username);
    cJSON_Delete(root);
    return ESP_OK;
}

bool lastfm_is_authenticated(void)
{
    return s_lfm.authenticated;
}

void lastfm_now_playing(const char *artist, const char *track,
                        const char *album, uint32_t duration_s)
{
    if (!s_lfm.authenticated) return;
    if (!artist || !artist[0] || !track || !track[0]) return;

    char dur[16];
    snprintf(dur, sizeof(dur), "%lu", (unsigned long)duration_s);

    const char *params[] = {
        "method",   "track.updateNowPlaying",
        "api_key",  s_lfm.cfg.api_key,
        "sk",       s_lfm.cfg.session_key,
        "artist",   artist,
        "track",    track,
        "album",    album ? album : "",
        "duration", dur,
    };

    cJSON *root = api_post(params, 7);
    if (root) {
        ESP_LOGI(TAG, "Now Playing: %s - %s", artist, track);
        cJSON_Delete(root);
    }
}

void lastfm_scrobble(const char *artist, const char *track,
                     const char *album, uint32_t duration_s,
                     uint32_t timestamp)
{
    if (!artist || !artist[0] || !track || !track[0]) return;

    lastfm_scrobble_entry_t entry = {0};
    strncpy(entry.artist, artist, sizeof(entry.artist) - 1);
    strncpy(entry.track, track, sizeof(entry.track) - 1);
    if (album) strncpy(entry.album, album, sizeof(entry.album) - 1);
    entry.duration_s = duration_s;
    entry.timestamp = timestamp;

    if (s_lfm.authenticated) {
        // Try to send immediately
        if (send_scrobble(&entry)) {
            ESP_LOGI(TAG, "Scrobbled: %s - %s", artist, track);
            return;
        }
    }

    // Queue for later
    queue_push(&entry);
    ESP_LOGI(TAG, "Queued scrobble: %s - %s (pending=%d)", artist, track, s_lfm.q_count);
}

void lastfm_flush_queue(void)
{
    if (!s_lfm.authenticated) return;

    int sent = 0;
    lastfm_scrobble_entry_t entry;
    while (queue_peek(&entry)) {
        if (!send_scrobble(&entry)) {
            ESP_LOGW(TAG, "Flush stopped: send failed after %d", sent);
            break;
        }
        queue_pop();
        sent++;
        vTaskDelay(pdMS_TO_TICKS(500));  // Rate limit
    }
    ESP_LOGI(TAG, "Flushed %d scrobbles, %d remaining", sent, s_lfm.q_count);
}

uint8_t lastfm_pending_count(void)
{
    return s_lfm.q_count;
}

//--------------------------------------------------------------------+
// CDC command handler
//--------------------------------------------------------------------+

void lastfm_handle_cdc_command(const char *sub, lastfm_print_fn_t print)
{
    if (!sub || !sub[0] || strcmp(sub, "help") == 0) {
        print("Last.fm commands:\r\n");
        print("  lastfm auth <api_key> <secret>  - Set API credentials\r\n");
        print("  lastfm login <user> <password>   - Authenticate\r\n");
        print("  lastfm status                    - Show state\r\n");
        print("  lastfm flush                     - Send pending scrobbles\r\n");
        print("  lastfm test                      - Scrobble test track\r\n");
        return;
    }

    if (strncmp(sub, "auth ", 5) == 0) {
        const char *args = sub + 5;
        while (*args == ' ') args++;

        // Parse: <api_key> <secret>
        char key[33] = {0}, secret[33] = {0};
        if (sscanf(args, "%32s %32s", key, secret) == 2) {
            lastfm_set_api_key(key, secret);
            print("[OK] API credentials saved\r\n");
        } else {
            print("Usage: lastfm auth <api_key> <shared_secret>\r\n");
        }
    } else if (strncmp(sub, "login ", 6) == 0) {
        const char *args = sub + 6;
        while (*args == ' ') args++;

        char user[64] = {0}, pass[64] = {0};
        if (sscanf(args, "%63s %63s", user, pass) == 2) {
            lastfm_authenticate(user, pass, print);
        } else {
            print("Usage: lastfm login <username> <password>\r\n");
        }
    } else if (strcmp(sub, "status") == 0) {
        print("Last.fm Status:\r\n");
        print("  API key: %s\r\n", s_lfm.cfg.api_key[0] ? "set" : "not set");
        print("  User: %s\r\n", s_lfm.cfg.username[0] ? s_lfm.cfg.username : "(none)");
        print("  Authenticated: %s\r\n", s_lfm.authenticated ? "YES" : "NO");
        print("  Pending scrobbles: %d\r\n", s_lfm.q_count);
    } else if (strcmp(sub, "flush") == 0) {
        if (!s_lfm.authenticated) {
            print("[ERR] Not authenticated\r\n");
            return;
        }
        if (s_lfm.q_count == 0) {
            print("No pending scrobbles\r\n");
            return;
        }
        print("Flushing %d scrobbles...\r\n", s_lfm.q_count);
        lastfm_flush_queue();
        print("[OK] %d remaining\r\n", s_lfm.q_count);
    } else if (strcmp(sub, "test") == 0) {
        if (!s_lfm.authenticated) {
            print("[ERR] Not authenticated\r\n");
            return;
        }
        uint32_t ts = (uint32_t)time(NULL);
        lastfm_scrobble("Lyra Test", "Test Track", "Test Album", 180, ts);
        print("[OK] Test scrobble sent/queued\r\n");
    } else {
        print("Unknown lastfm command: %s\r\n", sub);
    }
}
