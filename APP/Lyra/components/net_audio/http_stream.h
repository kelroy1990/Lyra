#pragma once

// Private HTTP streaming layer for net_audio component.
// Not exposed in public include dir.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_http_client.h"

//--------------------------------------------------------------------+
// Detected codec type from Content-Type or URL extension
//--------------------------------------------------------------------+

typedef enum {
    HTTP_CODEC_UNKNOWN = 0,
    HTTP_CODEC_FLAC,
    HTTP_CODEC_MP3,
    HTTP_CODEC_WAV,
    HTTP_CODEC_AAC,
    HTTP_CODEC_OGG,
} http_codec_type_t;

//--------------------------------------------------------------------+
// HTTP stream handle
//--------------------------------------------------------------------+

typedef struct {
    esp_http_client_handle_t client;

    // ICY metadata (internet radio)
    uint32_t icy_metaint;           // 0 = not an ICY stream
    uint32_t icy_bytes_since_meta;  // bytes consumed since last metadata block
    char     icy_title[256];        // Current StreamTitle from ICY metadata

    // Byte position tracking (for tell callback)
    int64_t  position;              // Current audio byte position (excl. metadata)

    // Stream properties from headers
    http_codec_type_t codec;
    char content_type[64];
    int  status_code;

    // Error flag — set on network failure
    bool error;
} http_stream_t;

//--------------------------------------------------------------------+
// API
//--------------------------------------------------------------------+

// Open HTTP connection to URL, parse headers, detect codec.
// hint:    codec hint string (may be NULL for autodetect from Content-Type).
// referer: Referer header (may be NULL). Required by some CDNs (Radio France,
//          some BBC streams) that check hotlink protection.
// Returns ESP_OK on success (2xx response).
esp_err_t http_stream_open(http_stream_t *hs, const char *url,
                            const char *hint, const char *referer);

// Read exactly n bytes of audio data (transparent ICY metadata stripping).
// Blocks until all bytes are available, EOF, or error.
// Returns actual bytes read (< n means EOF or error).
size_t http_stream_read(http_stream_t *hs, void *buf, size_t n);

// Seek — only SEEK_CUR forward supported for HTTP streams.
// Any other seek returns false. Decoders handle this gracefully for streaming.
bool http_stream_seek(http_stream_t *hs, int64_t offset, int origin);

// Return current audio byte position (for tell callbacks)
int64_t http_stream_tell(http_stream_t *hs);

// Close and free resources
void http_stream_close(http_stream_t *hs);

// Detect codec from Content-Type header and/or URL extension
http_codec_type_t http_stream_detect_codec(const char *content_type,
                                            const char *url,
                                            const char *hint);

// String name of codec type
const char *http_codec_name(http_codec_type_t codec);
