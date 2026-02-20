#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

//--------------------------------------------------------------------+
// Track metadata structure (internal representation)
//--------------------------------------------------------------------+

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char album_artist[256];
    char date[32];              // "2024" or "2024-03-15"
    char genre[64];
    uint32_t track_number;
    uint32_t disc_number;
    uint32_t total_tracks;
    uint32_t total_discs;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint8_t  channels;
    uint32_t bitrate_kbps;

    // MusicBrainz identifiers (from tags or downloaded)
    char musicbrainz_release_id[64];
    char musicbrainz_track_id[64];    // Recording ID
    char isrc[16];                    // International Standard Recording Code

    // Cover art state
    bool has_embedded_cover;          // Embedded in the audio file itself
    bool has_external_cover;          // cover.jpg exists in same directory

    // Download source (for informational purposes)
    char meta_source[32];             // "local", "musicbrainz", "lastfm", "acoustid"
} lyra_track_meta_t;

//--------------------------------------------------------------------+
// Callback types
//--------------------------------------------------------------------+

// Progress callback: percent 0-100
typedef void (*meta_progress_cb_t)(uint8_t percent);

// Print function (for CDC output)
typedef void (*meta_print_fn_t)(const char *fmt, ...);

// Metadata service types (for API key management)
typedef enum {
    META_SERVICE_LASTFM = 0,
    META_SERVICE_ACOUSTID,
} meta_service_t;

//--------------------------------------------------------------------+
// Local tag reading (no network)
//--------------------------------------------------------------------+

// Read metadata from audio file tags only (ID3v2, VORBISCOMMENT, etc.)
// Does not require network. Fast.
esp_err_t meta_read_local(const char *filepath, lyra_track_meta_t *out);

//--------------------------------------------------------------------+
// SD storage — file conventions
//--------------------------------------------------------------------+

// cover.jpg path for a given directory (builds path string, does NOT check existence)
// out_path must be at least PATH_MAX bytes
void meta_get_cover_path(const char *dir_path, char *out_path, size_t out_size);

// Check if .meta/ directory with album.json exists in dir_path
bool meta_has_downloaded(const char *dir_path);

//--------------------------------------------------------------------+
// Online metadata download
//--------------------------------------------------------------------+

// Download metadata for the album directory containing file_or_dir_path.
// Saves: <dir>/.meta/album.json, <dir>/.meta/<track>.json, <dir>/cover.jpg
// Uses MusicBrainz + Cover Art Archive + Last.fm (fallback).
// AcoustID fingerprinting used when no tags available.
// Rate limiting: 1100ms between MusicBrainz requests (API limit).
esp_err_t meta_download_album(const char *file_or_dir_path,
                               meta_progress_cb_t progress_cb);

// Download metadata for a single track (only .meta/<track>.json, no cover)
esp_err_t meta_download_track(const char *filepath,
                               meta_progress_cb_t progress_cb);

// Recursively download metadata for all albums under root_path.
// Skips directories that already have .meta/album.json (unless force=true).
// Long-running: rate-limited to 1 request/second for MusicBrainz.
esp_err_t meta_download_library(const char *root_path, bool force,
                                 meta_progress_cb_t progress_cb);

// Cancel in-progress download (safe to call from any context)
void meta_cancel_download(void);

// Get current download progress (0-100), or 0 if not downloading
uint8_t meta_get_download_progress(void);

//--------------------------------------------------------------------+
// API key management (persisted in NVS lyra_cfg partition)
//--------------------------------------------------------------------+

esp_err_t meta_set_api_key(meta_service_t service, const char *key);
esp_err_t meta_get_api_key(meta_service_t service, char *buf, size_t len);

//--------------------------------------------------------------------+
// CDC command handler
//--------------------------------------------------------------------+

void meta_handle_cdc_command(const char *subcommand, meta_print_fn_t print_fn);
