#include "http_stream.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_crt_bundle.h" // for HTTPS streams (attach system cert bundle)
#include "lwip/sockets.h"   // lwip_getaddrinfo — LWIP_COMPAT_SOCKETS=0 requires explicit lwip_ prefix

static const char *TAG = "http_stream";

//--------------------------------------------------------------------+
// Codec detection
//--------------------------------------------------------------------+

http_codec_type_t http_stream_detect_codec(const char *content_type,
                                            const char *url,
                                            const char *hint)
{
    // Hint takes priority
    if (hint && *hint) {
        if (strcasecmp(hint, "flac") == 0) return HTTP_CODEC_FLAC;
        if (strcasecmp(hint, "mp3")  == 0) return HTTP_CODEC_MP3;
        if (strcasecmp(hint, "wav")  == 0) return HTTP_CODEC_WAV;
        if (strcasecmp(hint, "aac")  == 0) return HTTP_CODEC_AAC;
        if (strcasecmp(hint, "ogg")  == 0) return HTTP_CODEC_OGG;
    }

    // Content-Type
    if (content_type && *content_type) {
        if (strstr(content_type, "flac"))                return HTTP_CODEC_FLAC;
        if (strstr(content_type, "mpeg"))                return HTTP_CODEC_MP3;
        if (strstr(content_type, "mp3"))                 return HTTP_CODEC_MP3;
        if (strstr(content_type, "wav"))                 return HTTP_CODEC_WAV;
        if (strstr(content_type, "aac"))                 return HTTP_CODEC_AAC;
        if (strstr(content_type, "mp4"))                 return HTTP_CODEC_AAC;
        if (strstr(content_type, "ogg"))                 return HTTP_CODEC_OGG;
        if (strstr(content_type, "vorbis"))              return HTTP_CODEC_OGG;
        // ICY streams often send audio/mpeg for MP3
        if (strstr(content_type, "audio/mpeg"))          return HTTP_CODEC_MP3;
    }

    // URL fallback: check both the file extension AND the full last path segment.
    // Handles both classic /stream.mp3 and ICY-style /stream-128-mp3 URLs.
    if (url && *url) {
        // 1. Classic extension check
        const char *dot = strrchr(url, '.');
        if (dot) {
            const char *ext = dot + 1;
            char ext_clean[8] = {0};
            size_t i;
            for (i = 0; i < sizeof(ext_clean) - 1 && ext[i] && ext[i] != '?' && ext[i] != '#'; i++) {
                ext_clean[i] = ext[i];
            }
            if (strcasecmp(ext_clean, "flac") == 0) return HTTP_CODEC_FLAC;
            if (strcasecmp(ext_clean, "mp3")  == 0) return HTTP_CODEC_MP3;
            if (strcasecmp(ext_clean, "wav")  == 0) return HTTP_CODEC_WAV;
            if (strcasecmp(ext_clean, "aac")  == 0) return HTTP_CODEC_AAC;
            if (strcasecmp(ext_clean, "m4a")  == 0) return HTTP_CODEC_AAC;
            if (strcasecmp(ext_clean, "ogg")  == 0) return HTTP_CODEC_OGG;
        }

        // 2. Last URL path segment keyword search (handles /groovesalad-128-mp3).
        //    Lowercase the segment to allow case-insensitive substring match.
        const char *slash = strrchr(url, '/');
        const char *seg   = slash ? slash + 1 : url;
        char seg_lc[64]   = {0};
        size_t j;
        for (j = 0; j < sizeof(seg_lc) - 1 && seg[j] && seg[j] != '?'; j++) {
            seg_lc[j] = (char)tolower((unsigned char)seg[j]);
        }
        if (strstr(seg_lc, "flac"))                           return HTTP_CODEC_FLAC;
        if (strstr(seg_lc, "mp3") || strstr(seg_lc, "mpeg")) return HTTP_CODEC_MP3;
        if (strstr(seg_lc, "wav"))                            return HTTP_CODEC_WAV;
        if (strstr(seg_lc, "aac") || strstr(seg_lc, "m4a"))  return HTTP_CODEC_AAC;
        if (strstr(seg_lc, "ogg") || strstr(seg_lc, "vorbis")) return HTTP_CODEC_OGG;
    }

    return HTTP_CODEC_UNKNOWN;
}

const char *http_codec_name(http_codec_type_t codec)
{
    switch (codec) {
        case HTTP_CODEC_FLAC: return "flac";
        case HTTP_CODEC_MP3:  return "mp3";
        case HTTP_CODEC_WAV:  return "wav";
        case HTTP_CODEC_AAC:  return "aac";
        case HTTP_CODEC_OGG:  return "ogg";
        default:              return "unknown";
    }
}

//--------------------------------------------------------------------+
// Header parsing helpers
//--------------------------------------------------------------------+

// Extract a specific header value from the HTTP client (stored in hs after open)
static void parse_icy_header(http_stream_t *hs, esp_http_client_handle_t client)
{
    // icy-metaint: interval between ICY metadata blocks (bytes of audio)
    // esp_http_client_get_header sets *value to internal buffer — do not free
    char *val = NULL;
    esp_http_client_get_header(client, "icy-metaint", &val);
    if (val && val[0]) {
        hs->icy_metaint = (uint32_t)atoi(val);
        hs->icy_bytes_since_meta = 0;
        if (hs->icy_metaint > 0) {
            ESP_LOGI(TAG, "ICY stream detected: metaint=%lu", (unsigned long)hs->icy_metaint);
        }
    }
}

//--------------------------------------------------------------------+
// Open HTTP connection
//--------------------------------------------------------------------+

esp_err_t http_stream_open(http_stream_t *hs, const char *url,
                            const char *hint, const char *referer)
{
    memset(hs, 0, sizeof(*hs));

    esp_http_client_config_t cfg = {
        .url                  = url,
        .timeout_ms           = 30000,
        .buffer_size          = 8192,
        .buffer_size_tx       = 512,
        .keep_alive_enable    = true,
        .max_redirection_count = 10,
        // Broadly accepted UA — "Lyra-Player/1.0" gets 403 from Radio France CDN
        // and other stations that whitelist known player UAs.
        .user_agent           = "foobar2000/1.6.14",
        // HTTPS: attach the ESP-IDF system certificate bundle so TLS works
        // for streams served over https:// (Radio France, Qobuz, etc.)
        .crt_bundle_attach    = esp_crt_bundle_attach,
    };

    hs->client = esp_http_client_init(&cfg);
    if (!hs->client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    // Request ICY metadata for internet radio
    esp_http_client_set_header(hs->client, "Icy-MetaData", "1");
    // Accept any content type (some servers return 406 without this)
    esp_http_client_set_header(hs->client, "Accept", "*/*");
    // Referer: required by CDNs that enforce hotlink protection (Radio France, BBC...)
    if (referer && *referer) {
        esp_http_client_set_header(hs->client, "Referer", referer);
        ESP_LOGI(TAG, "Referer: %s", referer);
    }

    esp_err_t err = esp_http_client_open(hs->client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(hs->client);
        hs->client = NULL;
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(hs->client);
    hs->status_code = esp_http_client_get_status_code(hs->client);

    ESP_LOGI(TAG, "HTTP %d, content-length=%lld, url=%s",
             hs->status_code, content_length, url);

    if (hs->status_code < 200 || hs->status_code >= 300) {
        ESP_LOGE(TAG, "HTTP error status: %d", hs->status_code);
        esp_http_client_cleanup(hs->client);
        hs->client = NULL;
        return ESP_FAIL;
    }

    // Get Content-Type.
    // Icecast/SHOUTcast servers send lowercase "content-type" (RFC 7230 allows this).
    // esp_http_client response header lookup may be case-sensitive depending on version,
    // so try both capitalizations.
    {
        char *ct = NULL;
        esp_http_client_get_header(hs->client, "Content-Type", &ct);
        if (!ct || !*ct) {
            esp_http_client_get_header(hs->client, "content-type", &ct);
        }
        if (ct && *ct) {
            strncpy(hs->content_type, ct, sizeof(hs->content_type) - 1);
        }
    }

    // ICY headers
    parse_icy_header(hs, hs->client);

    // Detect codec
    hs->codec = http_stream_detect_codec(hs->content_type, url, hint);

    // ICY fallback: SHOUTcast/Icecast streams that send no Content-Type and have
    // no codec keyword in the URL are almost always MP3 (SHOUTcast convention).
    if (hs->codec == HTTP_CODEC_UNKNOWN && hs->icy_metaint > 0) {
        hs->codec = HTTP_CODEC_MP3;
        ESP_LOGW(TAG, "ICY stream with undetected codec → assuming MP3");
    }

    ESP_LOGI(TAG, "Content-Type: '%s', codec: %s", hs->content_type, http_codec_name(hs->codec));

    hs->position = 0;
    hs->error = false;
    return ESP_OK;
}

//--------------------------------------------------------------------+
// ICY metadata block parser
//--------------------------------------------------------------------+

// Call when we've reached the metaint boundary.
// Reads and parses the ICY metadata block, updating hs->icy_title.
static void icy_parse_metadata_block(http_stream_t *hs)
{
    // First byte: metadata block length in units of 16 bytes
    uint8_t len_byte = 0;
    int rd = esp_http_client_read(hs->client, (char *)&len_byte, 1);
    if (rd <= 0) {
        hs->error = true;
        return;
    }

    uint32_t meta_len = (uint32_t)len_byte * 16;
    if (meta_len == 0) return;  // Empty metadata block

    // Allocate and read metadata text
    char *meta_buf = malloc(meta_len + 1);
    if (!meta_buf) {
        // Skip without parsing
        char skip[64];
        uint32_t remaining = meta_len;
        while (remaining > 0) {
            uint32_t chunk = remaining < sizeof(skip) ? remaining : sizeof(skip);
            int skipped = esp_http_client_read(hs->client, skip, chunk);
            if (skipped <= 0) { hs->error = true; return; }
            remaining -= skipped;
        }
        return;
    }

    uint32_t remaining = meta_len;
    uint32_t offset = 0;
    while (remaining > 0) {
        int got = esp_http_client_read(hs->client, meta_buf + offset, remaining);
        if (got <= 0) { hs->error = true; free(meta_buf); return; }
        offset += got;
        remaining -= got;
    }
    meta_buf[meta_len] = '\0';

    // Parse StreamTitle='...';
    char *title_start = strstr(meta_buf, "StreamTitle='");
    if (title_start) {
        title_start += 13;  // Skip "StreamTitle='"
        char *title_end = strstr(title_start, "';");
        if (!title_end) title_end = strchr(title_start, ';');
        size_t title_len = title_end ? (size_t)(title_end - title_start) : strlen(title_start);
        if (title_len >= sizeof(hs->icy_title)) title_len = sizeof(hs->icy_title) - 1;
        memcpy(hs->icy_title, title_start, title_len);
        hs->icy_title[title_len] = '\0';
        if (hs->icy_title[0]) {
            ESP_LOGI(TAG, "ICY: \"%s\"", hs->icy_title);
        }
    }

    free(meta_buf);
}

//--------------------------------------------------------------------+
// Read audio bytes (ICY-transparent)
//--------------------------------------------------------------------+

size_t http_stream_read(http_stream_t *hs, void *buf, size_t n)
{
    if (!hs->client || hs->error) return 0;

    uint8_t *dst = (uint8_t *)buf;
    size_t total_read = 0;

    while (total_read < n) {
        size_t remaining = n - total_read;

        if (hs->icy_metaint > 0) {
            // How many audio bytes until the next metadata block?
            uint32_t until_meta = hs->icy_metaint - hs->icy_bytes_since_meta;
            if (until_meta == 0) {
                icy_parse_metadata_block(hs);
                if (hs->error) break;
                hs->icy_bytes_since_meta = 0;
                until_meta = hs->icy_metaint;
            }
            // Read no more than until_meta bytes at once
            if (remaining > until_meta) remaining = until_meta;
        }

        int rd = esp_http_client_read(hs->client, (char *)(dst + total_read), remaining);
        if (rd <= 0) {
            // EOF or network error
            if (rd < 0) {
                ESP_LOGW(TAG, "HTTP read error: %d", rd);
                hs->error = true;
            }
            break;
        }

        total_read += rd;
        hs->position += rd;
        if (hs->icy_metaint > 0) {
            hs->icy_bytes_since_meta += rd;
        }
    }

    return total_read;
}

//--------------------------------------------------------------------+
// Seek (limited support for HTTP streams)
//--------------------------------------------------------------------+

bool http_stream_seek(http_stream_t *hs, int64_t offset, int origin)
{
    // HTTP streaming: only forward SEEK_CUR is possible (skip bytes)
    // SEEK_SET to 0 is treated as "can't seek" (would need to reconnect)
    if (origin == SEEK_CUR && offset > 0) {
        // Skip forward by reading and discarding bytes
        char discard[256];
        int64_t remaining = offset;
        while (remaining > 0) {
            size_t chunk = remaining < (int64_t)sizeof(discard) ? (size_t)remaining : sizeof(discard);
            size_t got = http_stream_read(hs, discard, chunk);
            if (got == 0) return false;
            remaining -= (int64_t)got;
        }
        return true;
    }
    // All other seeks fail — decoder falls back to sequential read
    return false;
}

int64_t http_stream_tell(http_stream_t *hs)
{
    return hs->position;
}

//--------------------------------------------------------------------+
// Close
//--------------------------------------------------------------------+

void http_stream_close(http_stream_t *hs)
{
    if (hs->client) {
        esp_http_client_close(hs->client);
        esp_http_client_cleanup(hs->client);
        hs->client = NULL;
    }
    hs->error = false;
    hs->position = 0;
}
