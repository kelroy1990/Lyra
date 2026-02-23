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
 * Data getters (implementation swapped at link time)
 * ----------------------------------------------------------------------- */

ui_now_playing_t    ui_data_get_now_playing(void);
ui_system_status_t  ui_data_get_system_status(void);
ui_browser_data_t   ui_data_get_browser(void);

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
