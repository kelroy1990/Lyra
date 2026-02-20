#include "spotify.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "spotify";

// ---------------------------------------------------------------------------
// F8-E Stub — cspot integration pending
//
// To complete this component:
//
// 1. Add cspot as git submodule:
//    cd components/spotify
//    git submodule add https://github.com/feelfreelinux/cspot vendor/cspot
//
// 2. Add tremor (fixed-point Vorbis decoder):
//    Download tremor sources from https://gitlab.xiph.org/xiph/tremor
//    Place in components/spotify/tremor/
//
// 3. Implement spotify_glue.cpp:
//    - Wrap cspot::SpircController with ESP-IDF task
//    - Audio sink: Ogg Vorbis → tremor → int16 stereo → StreamBuffer
//    - ZeroConf: use mdns component for _spotify-connect._tcp advertisement
//
// 4. CMakeLists.txt:
//    - Enable C++17: idf_component_register(...) + set_property(... CXX_STANDARD 17)
//    - Link cspot sources
//    - Link tremor sources
//    - PRIV_REQUIRES: mdns, nvs_flash, net_audio, log, freertos
//
// 5. NVS storage for OAuth tokens (lyra_cfg partition, namespace "spotify")
//
// Reference: managed_components/espressif__esp_hosted/examples/host_bluedroid_host_only/
//   shows pattern for C++ glue in C-dominant IDF projects.
//
// Audio format: Ogg Vorbis 320kbps → 44100Hz 16-bit stereo
//   (cspot only supports Premium accounts, which get OGG 320kbps)
// ---------------------------------------------------------------------------

static spotify_status_t s_status = {0};

esp_err_t spotify_init(const char *device_name, const spotify_audio_cbs_t *cbs)
{
    (void)device_name;
    (void)cbs;
    ESP_LOGW(TAG, "Spotify Connect not yet implemented (F8-E stub)");
    ESP_LOGW(TAG, "Add cspot submódulo: git submodule add "
                  "https://github.com/feelfreelinux/cspot "
                  "components/spotify/vendor/cspot");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t spotify_start(void)
{
    ESP_LOGW(TAG, "Spotify Connect stub — not started");
    return ESP_ERR_NOT_SUPPORTED;
}

void spotify_stop(void)
{
    // noop
}

void spotify_send_pause(void)
{
    // noop — not connected
}

void spotify_send_play(void)
{
    // noop — not connected
}

bool spotify_is_active(void)
{
    return false;
}

spotify_status_t spotify_get_status(void)
{
    return s_status;
}

esp_err_t spotify_logout(void)
{
    // Clear NVS credentials when implemented
    return ESP_ERR_NOT_SUPPORTED;
}

void spotify_handle_cdc_command(const char *subcommand, spotify_print_fn_t print_fn)
{
    while (*subcommand == ' ') subcommand++;

    if (strcmp(subcommand, "status") == 0) {
        print_fn("Spotify: not implemented (F8-E stub)\r\n");
        print_fn("Requires cspot library — see components/spotify/spotify.c for setup\r\n");
    } else if (strcmp(subcommand, "logout") == 0) {
        print_fn("Spotify: not implemented\r\n");
    } else {
        print_fn("Spotify commands: spotify status, spotify logout\r\n");
    }
}
