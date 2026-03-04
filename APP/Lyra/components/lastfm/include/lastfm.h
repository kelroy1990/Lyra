#ifndef LASTFM_H
#define LASTFM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*lastfm_print_fn_t)(const char *fmt, ...);

/**
 * Initialize Last.fm client. Loads API key + session from NVS.
 */
esp_err_t lastfm_init(void);

/**
 * Set API credentials (persist to NVS).
 */
void lastfm_set_api_key(const char *api_key, const char *shared_secret);

/**
 * Authenticate with Last.fm (mobile auth → session key).
 * Session key is persisted to NVS on success.
 */
esp_err_t lastfm_authenticate(const char *username, const char *password,
                               lastfm_print_fn_t print);

/**
 * Check if authenticated (has valid session key).
 */
bool lastfm_is_authenticated(void);

/**
 * Update "Now Playing" on Last.fm profile.
 */
void lastfm_now_playing(const char *artist, const char *track,
                        const char *album, uint32_t duration_s);

/**
 * Submit a scrobble (track listened). Queued offline if no WiFi.
 * timestamp: Unix epoch seconds when playback started.
 */
void lastfm_scrobble(const char *artist, const char *track,
                     const char *album, uint32_t duration_s,
                     uint32_t timestamp);

/**
 * Flush pending scrobble queue (send all queued entries).
 */
void lastfm_flush_queue(void);

/**
 * Get number of pending scrobbles in queue.
 */
uint8_t lastfm_pending_count(void);

/**
 * CDC command handler. Subcommands:
 *   auth <api_key> <secret>     - Set API credentials
 *   login <user> <password>     - Authenticate
 *   status                      - Show auth state + pending queue
 *   flush                       - Force send pending scrobbles
 *   test                        - Scrobble a test track
 */
void lastfm_handle_cdc_command(const char *sub, lastfm_print_fn_t print);

#ifdef __cplusplus
}
#endif

#endif /* LASTFM_H */
