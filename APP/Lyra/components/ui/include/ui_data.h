#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * ui_data.h — Data abstraction layer between UI and system.
 *
 * The UI code (LVGL screens) reads all information through these getters.
 * Two implementations exist:
 *   - ui_data_esp.c   : reads from real subsystems (sd_player, audio_source, etc.)
 *   - ui_data_mock.c  : returns static mock data (PC simulator)
 */

/* -----------------------------------------------------------------------
 * Now Playing
 * ----------------------------------------------------------------------- */

typedef enum {
    UI_PLAYBACK_STOPPED,
    UI_PLAYBACK_PLAYING,
    UI_PLAYBACK_PAUSED,
} ui_playback_state_t;

typedef enum {
    UI_SOURCE_NONE,
    UI_SOURCE_USB,
    UI_SOURCE_SD,
    UI_SOURCE_NET,
    UI_SOURCE_BLUETOOTH,
} ui_audio_source_t;

typedef struct {
    char     title[128];
    char     artist[128];
    char     album[128];
    char     format_name[16];       /* "FLAC", "ALAC", "DSD128", "MP3", etc. */
    uint32_t sample_rate;           /* e.g. 96000 */
    uint8_t  bits_per_sample;       /* e.g. 24 */
    uint32_t duration_ms;           /* Total duration (0 = unknown) */
    uint32_t position_ms;           /* Current playback position */
    float    gain_db;               /* ReplayGain applied (0.0 = none) */
    bool     is_dsd;                /* true if DSD/DoP (affects display) */
    ui_playback_state_t state;
    ui_audio_source_t   source;
} ui_now_playing_t;

/* -----------------------------------------------------------------------
 * EQ / DSP
 * ----------------------------------------------------------------------- */

#define UI_EQ_BANDS  5              /* 60 Hz, 250 Hz, 1 kHz, 4 kHz, 16 kHz */

#define UI_MAX_USER_PRESETS  8
#define UI_PRESET_NAME_LEN   16

typedef struct {
    char    name[UI_PRESET_NAME_LEN];
    int8_t  bands[UI_EQ_BANDS];
} ui_eq_user_preset_t;

typedef struct {
    ui_eq_user_preset_t presets[UI_MAX_USER_PRESETS];
    int count;
} ui_eq_presets_data_t;

/* -----------------------------------------------------------------------
 * System status
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t  volume;                /* 0–100 */
    uint8_t  battery_pct;           /* 0–100 */
    bool     battery_charging;
    bool     wifi_connected;
    char     wifi_ssid[33];
    int8_t   wifi_rssi;             /* dBm, 0 if not connected */
    bool     bt_connected;
    const char *dsp_preset;         /* "Rock", "Flat", "Jazz", etc. */
    bool     dsp_enabled;
    int8_t   eq_bands[UI_EQ_BANDS]; /* Per-band gain: -12 to +12 dB */
} ui_system_status_t;

/* -----------------------------------------------------------------------
 * File Browser
 * ----------------------------------------------------------------------- */

#define UI_BROWSER_MAX_ITEMS  128

typedef enum {
    UI_ENTRY_FOLDER,
    UI_ENTRY_FILE,
} ui_entry_type_t;

typedef struct {
    char name[128];             /* Display name (filename only, no path) */
    ui_entry_type_t type;       /* Folder or audio file */
} ui_browser_entry_t;

typedef struct {
    char path[256];                                 /* Current folder path */
    ui_browser_entry_t items[UI_BROWSER_MAX_ITEMS];
    int  item_count;                                /* Valid entries in items[] */
    bool at_root;                                   /* true when path is "/" */
} ui_browser_data_t;

/* -----------------------------------------------------------------------
 * WiFi scan results
 * ----------------------------------------------------------------------- */

#define UI_WIFI_MAX_APS  20

typedef enum {
    UI_WIFI_AUTH_OPEN,
    UI_WIFI_AUTH_SECURED,       /* WPA/WPA2/WPA3 — details hidden from UI */
} ui_wifi_auth_t;

typedef enum {
    UI_WIFI_IDLE,
    UI_WIFI_SCANNING,
    UI_WIFI_CONNECTING,
    UI_WIFI_CONNECTED,
    UI_WIFI_ERROR,
} ui_wifi_state_t;

typedef struct {
    char            ssid[33];
    int8_t          rssi;       /* dBm */
    uint8_t         channel;
    ui_wifi_auth_t  auth;
} ui_wifi_ap_t;

typedef struct {
    ui_wifi_state_t state;
    ui_wifi_ap_t    aps[UI_WIFI_MAX_APS];
    int             ap_count;
    char            connected_ssid[33];  /* Empty if not connected */
    int8_t          connected_rssi;
    char            error_msg[64];       /* Non-empty when state==ERROR */
} ui_wifi_scan_data_t;

/* -----------------------------------------------------------------------
 * Net Audio / Streaming
 * ----------------------------------------------------------------------- */

#define UI_NET_AUDIO_MAX_PRESETS  16

typedef enum {
    UI_NET_AUDIO_IDLE,
    UI_NET_AUDIO_CONNECTING,
    UI_NET_AUDIO_BUFFERING,
    UI_NET_AUDIO_PLAYING,
    UI_NET_AUDIO_PAUSED,
    UI_NET_AUDIO_ERROR,
} ui_net_audio_state_t;

typedef struct {
    char name[64];
    char url[256];
    char codec_hint[16];   /* "mp3", "flac", etc. */
} ui_radio_preset_t;

typedef struct {
    ui_net_audio_state_t state;
    char     stream_title[128];    /* ICY StreamTitle or station name */
    char     stream_url[256];      /* Currently playing URL */
    char     codec[16];            /* "FLAC", "MP3", "WAV" */
    uint32_t bitrate_kbps;         /* 0 if unknown */
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint32_t elapsed_ms;
    char     error_msg[64];
    /* Preset list */
    ui_radio_preset_t presets[UI_NET_AUDIO_MAX_PRESETS];
    int      preset_count;
} ui_net_audio_data_t;

/* -----------------------------------------------------------------------
 * USB DAC
 * ----------------------------------------------------------------------- */

typedef enum {
    UI_USB_DAC_DISCONNECTED,
    UI_USB_DAC_CONNECTED,       /* Connected but idle */
    UI_USB_DAC_STREAMING,       /* Actively receiving audio */
} ui_usb_dac_state_t;

typedef struct {
    ui_usb_dac_state_t state;
    bool     enabled;           /* Whether USB DAC mode is on */
    uint32_t sample_rate;       /* Current sample rate (0 if not streaming) */
    uint8_t  bits_per_sample;   /* 16, 24, 32 */
    bool     is_dsd;            /* DSD over USB */
    uint32_t dsd_rate;          /* DSD64=2822400, DSD128=5644800, etc. */
    char     format_str[32];    /* "PCM 384kHz/32-bit" or "DSD256" */
    float    level_db_l;        /* Left channel dBFS (-inf to 0) */
    float    level_db_r;        /* Right channel dBFS */
} ui_usb_dac_data_t;

/* -----------------------------------------------------------------------
 * Playback Queue
 * ----------------------------------------------------------------------- */

#define UI_QUEUE_MAX_ITEMS  128

typedef struct {
    char     title[128];
    char     artist[64];
    uint32_t duration_ms;
} ui_queue_item_t;

typedef struct {
    ui_queue_item_t items[UI_QUEUE_MAX_ITEMS];
    int  count;
    int  current_index;     /* -1 if empty */
    bool shuffle_enabled;
} ui_queue_data_t;

/* -----------------------------------------------------------------------
 * Device / System Info
 * ----------------------------------------------------------------------- */

typedef struct {
    /* Device */
    char     model[32];             /* "Lyra DAP" */
    char     dac[32];               /* "ES9039Q2M" */
    char     display_res[16];       /* "720 x 1280" */
    /* Firmware */
    char     fw_version[32];        /* "1.0.0-dev" */
    char     build_date[32];        /* __DATE__ */
    char     idf_version[16];       /* "v5.5.2" */
    /* Hardware */
    char     cpu[48];               /* "ESP32-P4 RISC-V 400MHz" */
    uint32_t internal_ram_used_kb;
    uint32_t internal_ram_total_kb;
    uint32_t psram_used_kb;
    uint32_t psram_total_kb;
    uint32_t flash_total_mb;
    /* Storage */
    char     sd_capacity[16];       /* "64 GB" */
    char     sd_used[16];           /* "23.4 GB" */
    char     sd_free[16];           /* "40.6 GB" */
    char     sd_filesystem[16];     /* "exFAT" */
    char     current_file[256];
    /* Connectivity */
    char     wifi_mac[24];
    char     bt_mac[24];
    char     ip_address[20];        /* "192.168.1.42" or "N/A" */
    /* System */
    uint32_t uptime_seconds;
    int8_t   cpu_temp_c;            /* -128 if unavailable */
    char     audio_source[16];      /* "SD Card" etc. */
} ui_device_info_t;

/* -----------------------------------------------------------------------
 * Qobuz Hi-Fi Streaming
 * ----------------------------------------------------------------------- */

#define UI_QOBUZ_MAX_RESULTS   20
#define UI_QOBUZ_MAX_PROFILES   4

typedef enum {
    UI_QOBUZ_AUTH_IDLE,
    UI_QOBUZ_AUTH_LOGGING_IN,
    UI_QOBUZ_AUTH_OK,
    UI_QOBUZ_AUTH_FAILED,
} ui_qobuz_auth_state_t;

typedef enum {
    UI_QOBUZ_VIEW_PROFILES,        /* Saved account list (first screen) */
    UI_QOBUZ_VIEW_PROFILE_EDIT,    /* Add / edit account form */
    UI_QOBUZ_VIEW_RESULTS,         /* Search + results (browse mode) */
} ui_qobuz_view_t;

typedef struct {
    char id[16];            /* "q1", "q2", etc. */
    char name[64];          /* Display name: "Personal", "Family" */
    char email[128];
    bool connected;
} ui_qobuz_profile_t;

typedef struct {
    char     title[128];
    char     artist[128];
    char     album[128];
    char     track_id[32];
    uint32_t duration_s;        /* seconds */
    uint32_t sample_rate;       /* Hz, e.g. 192000 */
    uint8_t  bits_per_sample;   /* 16 or 24 */
    bool     is_hires;
} ui_qobuz_track_t;

typedef struct {
    ui_qobuz_auth_state_t auth_state;
    char                  username[64];
    char                  error_msg[128];
    bool                  searching;

    ui_qobuz_view_t       view;

    /* Connection profiles */
    ui_qobuz_profile_t    profiles[UI_QOBUZ_MAX_PROFILES];
    int                   profile_count;
    int                   editing_profile_idx;  /* -1 = new, 0..N-1 = editing */

    ui_qobuz_track_t      results[UI_QOBUZ_MAX_RESULTS];
    int                   result_count;
    char                  playing_track_id[32];
} ui_qobuz_data_t;

/* -----------------------------------------------------------------------
 * Subsonic / Navidrome / Airsonic
 * ----------------------------------------------------------------------- */

#define UI_SUBSONIC_MAX_ITEMS     20
#define UI_SUBSONIC_MAX_PROFILES   8

typedef enum {
    UI_SUBSONIC_AUTH_IDLE,
    UI_SUBSONIC_AUTH_CONNECTING,
    UI_SUBSONIC_AUTH_OK,
    UI_SUBSONIC_AUTH_FAILED,
} ui_subsonic_auth_state_t;

typedef enum {
    UI_SUBSONIC_VIEW_PROFILES,      /* Saved server list (first screen) */
    UI_SUBSONIC_VIEW_PROFILE_EDIT,  /* Add / edit server form */
    UI_SUBSONIC_VIEW_ALBUMS,        /* Album grid/list */
    UI_SUBSONIC_VIEW_TRACKS,        /* Tracks inside an album */
    UI_SUBSONIC_VIEW_SEARCH,        /* Search results */
} ui_subsonic_view_t;

typedef struct {
    char id[16];            /* "p1", "p2", etc. */
    char name[64];          /* Display name: "Home Navidrome" */
    char server_url[128];   /* "http://192.168.1.100:4533" */
    char username[64];
    bool connected;
} ui_subsonic_profile_t;

typedef struct {
    char     id[48];
    char     name[128];
    char     artist[128];
    uint16_t year;
    int      song_count;
    uint32_t duration_s;
} ui_subsonic_album_t;

typedef struct {
    char     id[48];
    char     title[128];
    char     artist[128];
    char     album[128];
    uint32_t duration_s;
    uint32_t sample_rate;
    uint16_t bit_rate;            /* kbps */
    uint8_t  bits_per_sample;
    char     suffix[8];           /* "flac", "mp3" */
    bool     is_hires;
} ui_subsonic_track_t;

typedef struct {
    ui_subsonic_auth_state_t auth_state;
    char                     server_url[128];
    char                     username[64];
    char                     error_msg[128];
    bool                     loading;

    ui_subsonic_view_t       view;

    /* Connection profiles */
    ui_subsonic_profile_t    profiles[UI_SUBSONIC_MAX_PROFILES];
    int                      profile_count;
    int                      editing_profile_idx;  /* -1 = new, 0..N-1 = editing */

    /* Album list (VIEW_ALBUMS) */
    ui_subsonic_album_t      albums[UI_SUBSONIC_MAX_ITEMS];
    int                      album_count;

    /* Track list (VIEW_TRACKS or VIEW_SEARCH) */
    ui_subsonic_track_t      tracks[UI_SUBSONIC_MAX_ITEMS];
    int                      track_count;
    char                     current_album_name[128];

    char                     playing_track_id[48];
} ui_subsonic_data_t;

/* -----------------------------------------------------------------------
 * Data getters (implementation swapped at link time)
 * ----------------------------------------------------------------------- */

ui_now_playing_t    ui_data_get_now_playing(void);
ui_system_status_t  ui_data_get_system_status(void);
ui_browser_data_t   ui_data_get_browser(void);
ui_wifi_scan_data_t ui_data_get_wifi_scan(void);
ui_net_audio_data_t ui_data_get_net_audio(void);
ui_usb_dac_data_t    ui_data_get_usb_dac(void);
ui_eq_presets_data_t ui_data_get_eq_presets(void);
ui_queue_data_t      ui_data_get_queue(void);
ui_device_info_t     ui_data_get_device_info(void);
ui_qobuz_data_t      ui_data_get_qobuz(void);
ui_subsonic_data_t   ui_data_get_subsonic(void);

/* -----------------------------------------------------------------------
 * Commands: UI → system actions
 * ----------------------------------------------------------------------- */

void ui_cmd_play_pause(void);
void ui_cmd_next(void);
void ui_cmd_prev(void);
void ui_cmd_seek(uint32_t position_ms);
void ui_cmd_set_volume(uint8_t vol);
void ui_cmd_set_dsp_preset(const char *name);
void ui_cmd_toggle_dsp(void);

/* EQ commands */
void ui_cmd_set_eq_band(int band, int8_t gain_db);  /* band: 0-4, gain: -12..+12 */

/* Browser commands */
void ui_cmd_browse_open(const char *folder_name);   /* Navigate into folder */
void ui_cmd_browse_back(void);                      /* Go to parent folder */
void ui_cmd_browse_play(const char *filename);      /* Play file from current folder */

/* WiFi commands */
void ui_cmd_wifi_scan(void);
void ui_cmd_wifi_connect(const char *ssid, const char *password);
void ui_cmd_wifi_disconnect(void);

/* Net Audio commands */
void ui_cmd_net_play(const char *url);
void ui_cmd_net_stop(void);
void ui_cmd_net_pause_resume(void);

/* USB DAC commands */
void ui_cmd_usb_dac_enable(bool enable);

/* EQ preset commands */
void ui_cmd_save_eq_preset(const char *name);
void ui_cmd_delete_eq_preset(const char *name);

/* Queue commands */
void ui_cmd_queue_jump(int index);
void ui_cmd_queue_remove(int index);
void ui_cmd_queue_clear(void);
void ui_cmd_queue_shuffle_toggle(void);

/* Qobuz commands */
void ui_cmd_qobuz_login(const char *email, const char *password);
void ui_cmd_qobuz_search(const char *query);
void ui_cmd_qobuz_play_track(const char *track_id);

/* Qobuz profile commands */
void ui_cmd_qobuz_save_profile(const char *id, const char *name,
                                const char *email, const char *pass);
void ui_cmd_qobuz_delete_profile(const char *id);
void ui_cmd_qobuz_select_profile(const char *id);
void ui_cmd_qobuz_edit_profile(int index);      /* -1 = new */
void ui_cmd_qobuz_cancel_edit(void);

/* Subsonic commands */
void ui_cmd_subsonic_connect(const char *url, const char *user, const char *pass);
void ui_cmd_subsonic_browse_albums(void);
void ui_cmd_subsonic_open_album(const char *album_id);
void ui_cmd_subsonic_search(const char *query);
void ui_cmd_subsonic_play_track(const char *track_id);
void ui_cmd_subsonic_back(void);

/* Subsonic profile commands */
void ui_cmd_subsonic_save_profile(const char *id, const char *name,
                                   const char *url, const char *user,
                                   const char *pass);
void ui_cmd_subsonic_delete_profile(const char *id);
void ui_cmd_subsonic_select_profile(const char *id);
void ui_cmd_subsonic_edit_profile(int index);   /* -1 = new */
void ui_cmd_subsonic_cancel_edit(void);
