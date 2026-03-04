#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_NVS_PARTITION          "lyra_cfg"
#define SETTINGS_MAX_SUBSONIC_PROFILES  8

//--------------------------------------------------------------------+
// Audio settings (namespace: "audio")
//--------------------------------------------------------------------+

typedef struct {
    uint8_t  preset;         ///< eq_preset_t enum value
    uint8_t  limiter_mode;   ///< dsp_limiter_mode_t: 0=hard_clip, 1=soft
    uint8_t  dsp_enabled;    ///< 0=bypass, 1=enabled
    uint8_t  volume;         ///< 0-100
    uint8_t  shuffle;        ///< 0=off, 1=on
    uint8_t  repeat_mode;    ///< repeat_mode_t: 0=off, 1=one, 2=all
} settings_audio_t;

//--------------------------------------------------------------------+
// WiFi settings (namespace: "wifi")
//--------------------------------------------------------------------+

typedef struct {
    char    ssid[33];
    char    password[65];
    uint8_t auto_connect;    ///< 0=no, 1=yes
} settings_wifi_t;

//--------------------------------------------------------------------+
// Subsonic profile (namespace: "sub_0" .. "sub_7")
//--------------------------------------------------------------------+

typedef struct {
    char     server_url[256];
    char     username[64];
    char     password[64];
    char     name[32];
    uint16_t max_bitrate;    ///< 0=native, 128/192/256/320
    uint8_t  active;         ///< is this the active profile?
} settings_subsonic_profile_t;

//--------------------------------------------------------------------+
// User EQ preset (namespace: "ueq_0" .. "ueq_7")
//--------------------------------------------------------------------+

#define SETTINGS_MAX_USER_EQ_SLOTS 8
#define SETTINGS_MAX_USER_EQ_BANDS 5

typedef struct {
    char    name[32];
    uint8_t num_bands;
    struct {
        uint8_t  type;       ///< biquad_type_t
        float    freq;
        float    gain;
        float    q;
    } bands[SETTINGS_MAX_USER_EQ_BANDS];
} settings_user_eq_t;

//--------------------------------------------------------------------+
// API
//--------------------------------------------------------------------+

esp_err_t settings_init(void);

// Audio
esp_err_t settings_save_audio(const settings_audio_t *cfg);
esp_err_t settings_load_audio(settings_audio_t *cfg);

// WiFi
esp_err_t settings_save_wifi(const settings_wifi_t *cfg);
esp_err_t settings_load_wifi(settings_wifi_t *cfg);

// Subsonic profiles (index 0..7)
esp_err_t settings_save_subsonic_profile(uint8_t index,
                                          const settings_subsonic_profile_t *profile);
esp_err_t settings_load_subsonic_profile(uint8_t index,
                                          settings_subsonic_profile_t *profile);
esp_err_t settings_delete_subsonic_profile(uint8_t index);
int       settings_get_active_subsonic_profile(void);

// User EQ presets (index 0..7)
esp_err_t settings_save_user_eq(uint8_t index, const settings_user_eq_t *eq);
esp_err_t settings_load_user_eq(uint8_t index, settings_user_eq_t *eq);

//--------------------------------------------------------------------+
// Last.fm settings (namespace: "lastfm")
//--------------------------------------------------------------------+

typedef struct {
    char api_key[33];
    char shared_secret[33];
    char session_key[33];
    char username[64];
} settings_lastfm_t;

esp_err_t settings_save_lastfm(const settings_lastfm_t *cfg);
esp_err_t settings_load_lastfm(settings_lastfm_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_STORE_H */
