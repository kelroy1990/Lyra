/*
 * ui_data_mock.c — Mock data provider for the PC simulator.
 *
 * Returns static fake data so the UI can be developed and tested
 * without an ESP32-P4 or any real audio subsystem.
 */

#include "ui_data.h"
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * EQ presets (dB gain per band: 60Hz, 250Hz, 1kHz, 4kHz, 16kHz)
 * ----------------------------------------------------------------------- */

static const int8_t eq_flat[UI_EQ_BANDS]  = {  0,  0,  0,  0,  0 };
static const int8_t eq_rock[UI_EQ_BANDS]  = {  5,  3, -1,  4,  6 };
static const int8_t eq_jazz[UI_EQ_BANDS]  = {  3, -1,  2,  4,  3 };
static const int8_t eq_bass[UI_EQ_BANDS]  = {  8,  5,  0, -1, -2 };

/* -----------------------------------------------------------------------
 * Simulated state (modified by UI commands)
 * ----------------------------------------------------------------------- */

static ui_now_playing_t s_np = {
    .title           = "Wish You Were Here",
    .artist          = "Pink Floyd",
    .album           = "Wish You Were Here (Remastered)",
    .format_name     = "FLAC",
    .sample_rate     = 96000,
    .bits_per_sample = 24,
    .duration_ms     = 334000,   /* 5:34 */
    .position_ms     = 67000,    /* 1:07 */
    .gain_db         = -1.2f,
    .is_dsd          = false,
    .state           = UI_PLAYBACK_PLAYING,
    .source          = UI_SOURCE_SD,
};

static ui_system_status_t s_sys = {
    .volume           = 72,
    .battery_pct      = 82,
    .battery_charging = false,
    .wifi_connected   = true,
    .wifi_ssid        = "HomeNetwork",
    .wifi_rssi        = -52,
    .bt_connected     = false,
    .dsp_preset       = "Rock",
    .dsp_enabled      = true,
    .eq_bands         = { 5, 3, -1, 4, 6 },   /* Rock preset */
};

/* Preset names (for cycling) */
static const char *s_preset_names[] = { "Flat", "Rock", "Jazz", "Bass" };
static const int8_t *s_preset_curves[] = { eq_flat, eq_rock, eq_jazz, eq_bass };
#define NUM_PRESETS  4
static int s_preset_idx = 1;  /* Start on Rock */

/* -----------------------------------------------------------------------
 * Mock file browser data
 * ----------------------------------------------------------------------- */

static char s_browser_path[256] = "/";

/* Root "/" */
static const ui_browser_entry_t s_dir_root[] = {
    { "Music",    UI_ENTRY_FOLDER },
    { "Podcasts", UI_ENTRY_FOLDER },
};
#define DIR_ROOT_COUNT  2

/* /Music */
static const ui_browser_entry_t s_dir_music[] = {
    { "Pink Floyd",     UI_ENTRY_FOLDER },
    { "Steely Dan",     UI_ENTRY_FOLDER },
    { "Dire Straits",   UI_ENTRY_FOLDER },
    { "compilation.mp3", UI_ENTRY_FILE },
};
#define DIR_MUSIC_COUNT  4

/* /Music/Pink Floyd */
static const ui_browser_entry_t s_dir_pink_floyd[] = {
    { "01 - Shine On You Crazy Diamond (Parts I-V).flac",  UI_ENTRY_FILE },
    { "02 - Welcome to the Machine.flac",                  UI_ENTRY_FILE },
    { "03 - Have a Cigar.flac",                            UI_ENTRY_FILE },
    { "04 - Wish You Were Here.flac",                      UI_ENTRY_FILE },
    { "05 - Shine On You Crazy Diamond (Parts VI-IX).flac", UI_ENTRY_FILE },
};
#define DIR_PINK_FLOYD_COUNT  5

/* /Music/Steely Dan */
static const ui_browser_entry_t s_dir_steely_dan[] = {
    { "01 - Black Cow.flac",      UI_ENTRY_FILE },
    { "02 - Aja.flac",            UI_ENTRY_FILE },
    { "03 - Deacon Blues.flac",   UI_ENTRY_FILE },
    { "04 - Peg.flac",           UI_ENTRY_FILE },
    { "05 - Home at Last.flac",  UI_ENTRY_FILE },
    { "06 - I Got the News.flac", UI_ENTRY_FILE },
    { "07 - Josie.flac",         UI_ENTRY_FILE },
};
#define DIR_STEELY_DAN_COUNT  7

/* /Music/Dire Straits */
static const ui_browser_entry_t s_dir_dire_straits[] = {
    { "01 - So Far Away.dsf",           UI_ENTRY_FILE },
    { "02 - Money for Nothing.dsf",     UI_ENTRY_FILE },
    { "03 - Walk of Life.dsf",          UI_ENTRY_FILE },
    { "04 - Your Latest Trick.dsf",     UI_ENTRY_FILE },
    { "05 - Why Worry.dsf",             UI_ENTRY_FILE },
    { "06 - Ride Across the River.dsf",  UI_ENTRY_FILE },
    { "07 - The Man's Too Strong.dsf",   UI_ENTRY_FILE },
    { "08 - One World.dsf",             UI_ENTRY_FILE },
    { "09 - Brothers in Arms.dsf",      UI_ENTRY_FILE },
};
#define DIR_DIRE_STRAITS_COUNT  9

/* /Podcasts */
static const ui_browser_entry_t s_dir_podcasts[] = {
    { "episode_042.mp3", UI_ENTRY_FILE },
    { "episode_043.mp3", UI_ENTRY_FILE },
};
#define DIR_PODCASTS_COUNT  2

/* Helper: find the mock directory listing for current path */
static void get_mock_listing(const ui_browser_entry_t **out, int *count)
{
    if (strcmp(s_browser_path, "/") == 0) {
        *out = s_dir_root; *count = DIR_ROOT_COUNT;
    } else if (strcmp(s_browser_path, "/Music") == 0) {
        *out = s_dir_music; *count = DIR_MUSIC_COUNT;
    } else if (strcmp(s_browser_path, "/Music/Pink Floyd") == 0) {
        *out = s_dir_pink_floyd; *count = DIR_PINK_FLOYD_COUNT;
    } else if (strcmp(s_browser_path, "/Music/Steely Dan") == 0) {
        *out = s_dir_steely_dan; *count = DIR_STEELY_DAN_COUNT;
    } else if (strcmp(s_browser_path, "/Music/Dire Straits") == 0) {
        *out = s_dir_dire_straits; *count = DIR_DIRE_STRAITS_COUNT;
    } else if (strcmp(s_browser_path, "/Podcasts") == 0) {
        *out = s_dir_podcasts; *count = DIR_PODCASTS_COUNT;
    } else {
        *out = NULL; *count = 0;
    }
}

/* -----------------------------------------------------------------------
 * Data getters
 * ----------------------------------------------------------------------- */

ui_now_playing_t ui_data_get_now_playing(void)
{
    /* Advance position by ~200ms each call to simulate playback */
    if (s_np.state == UI_PLAYBACK_PLAYING && s_np.duration_ms > 0) {
        s_np.position_ms += 200;
        if (s_np.position_ms >= s_np.duration_ms)
            s_np.position_ms = 0;
    }
    return s_np;
}

ui_system_status_t ui_data_get_system_status(void)
{
    return s_sys;
}

/* -----------------------------------------------------------------------
 * Commands (mock: print + update local state)
 * ----------------------------------------------------------------------- */

void ui_cmd_play_pause(void)
{
    if (s_np.state == UI_PLAYBACK_PLAYING) {
        s_np.state = UI_PLAYBACK_PAUSED;
        printf("[MOCK] Paused\n");
    } else {
        s_np.state = UI_PLAYBACK_PLAYING;
        printf("[MOCK] Playing\n");
    }
}

void ui_cmd_next(void)
{
    printf("[MOCK] Next track\n");
    strncpy(s_np.title, "Money", sizeof(s_np.title));
    strncpy(s_np.artist, "Pink Floyd", sizeof(s_np.artist));
    strncpy(s_np.album, "The Dark Side of the Moon", sizeof(s_np.album));
    strncpy(s_np.format_name, "ALAC", sizeof(s_np.format_name));
    s_np.sample_rate     = 44100;
    s_np.bits_per_sample = 16;
    s_np.duration_ms     = 382000;
    s_np.position_ms     = 0;
    s_np.gain_db         = 0.0f;
}

void ui_cmd_prev(void)
{
    printf("[MOCK] Previous track\n");
    strncpy(s_np.title, "Wish You Were Here", sizeof(s_np.title));
    strncpy(s_np.artist, "Pink Floyd", sizeof(s_np.artist));
    strncpy(s_np.album, "Wish You Were Here (Remastered)", sizeof(s_np.album));
    strncpy(s_np.format_name, "FLAC", sizeof(s_np.format_name));
    s_np.sample_rate     = 96000;
    s_np.bits_per_sample = 24;
    s_np.duration_ms     = 334000;
    s_np.position_ms     = 0;
    s_np.gain_db         = -1.2f;
}

void ui_cmd_seek(uint32_t position_ms)
{
    printf("[MOCK] Seek to %lu ms\n", (unsigned long)position_ms);
    s_np.position_ms = position_ms;
}

void ui_cmd_set_volume(uint8_t vol)
{
    printf("[MOCK] Volume = %d%%\n", vol);
    s_sys.volume = vol;
}

void ui_cmd_set_dsp_preset(const char *name)
{
    printf("[MOCK] DSP preset = %s\n", name);
    /* Find preset by name and apply its EQ curve */
    for (int i = 0; i < NUM_PRESETS; i++) {
        if (strcmp(name, s_preset_names[i]) == 0) {
            s_preset_idx = i;
            s_sys.dsp_preset = s_preset_names[i];
            s_sys.dsp_enabled = true;
            memcpy(s_sys.eq_bands, s_preset_curves[i], UI_EQ_BANDS);
            return;
        }
    }
}

void ui_cmd_toggle_dsp(void)
{
    s_sys.dsp_enabled = !s_sys.dsp_enabled;
    printf("[MOCK] DSP %s\n", s_sys.dsp_enabled ? "ON" : "OFF");
    if (!s_sys.dsp_enabled)
        memcpy(s_sys.eq_bands, eq_flat, UI_EQ_BANDS);
    else
        memcpy(s_sys.eq_bands, s_preset_curves[s_preset_idx], UI_EQ_BANDS);
}

void ui_cmd_set_eq_band(int band, int8_t gain_db)
{
    if (band < 0 || band >= UI_EQ_BANDS) return;
    if (gain_db < -12) gain_db = -12;
    if (gain_db >  12) gain_db =  12;
    printf("[MOCK] EQ band %d = %d dB\n", band, gain_db);
    s_sys.eq_bands[band] = gain_db;
    s_sys.dsp_preset = "Custom";
    s_sys.dsp_enabled = true;
}

/* -----------------------------------------------------------------------
 * Browser data getter
 * ----------------------------------------------------------------------- */

ui_browser_data_t ui_data_get_browser(void)
{
    ui_browser_data_t br = {0};
    strncpy(br.path, s_browser_path, sizeof(br.path) - 1);
    br.at_root = (strcmp(s_browser_path, "/") == 0);

    const ui_browser_entry_t *entries;
    int count;
    get_mock_listing(&entries, &count);

    if (count > UI_BROWSER_MAX_ITEMS)
        count = UI_BROWSER_MAX_ITEMS;

    for (int i = 0; i < count; i++)
        br.items[i] = entries[i];
    br.item_count = count;

    return br;
}

/* -----------------------------------------------------------------------
 * Browser commands
 * ----------------------------------------------------------------------- */

void ui_cmd_browse_open(const char *folder_name)
{
    printf("[MOCK] Browse open: %s\n", folder_name);

    /* Append folder to current path */
    size_t len = strlen(s_browser_path);
    if (len == 1 && s_browser_path[0] == '/') {
        /* Root: just append name */
        snprintf(s_browser_path, sizeof(s_browser_path), "/%s", folder_name);
    } else {
        snprintf(s_browser_path + len, sizeof(s_browser_path) - len,
                 "/%s", folder_name);
    }
}

void ui_cmd_browse_back(void)
{
    printf("[MOCK] Browse back from: %s\n", s_browser_path);

    /* Strip last path component */
    char *last_slash = strrchr(s_browser_path, '/');
    if (last_slash && last_slash != s_browser_path) {
        *last_slash = '\0';
    } else {
        /* Already at root or one level deep → go to root */
        strcpy(s_browser_path, "/");
    }
}

void ui_cmd_browse_play(const char *filename)
{
    printf("[MOCK] Play file: %s/%s\n", s_browser_path, filename);

    /* Update now-playing mock data with the selected file */
    strncpy(s_np.title, filename, sizeof(s_np.title) - 1);
    s_np.title[sizeof(s_np.title) - 1] = '\0';

    /* Strip extension for display */
    char *dot = strrchr(s_np.title, '.');
    if (dot) *dot = '\0';

    strncpy(s_np.artist, "Unknown Artist", sizeof(s_np.artist));
    strncpy(s_np.album, s_browser_path, sizeof(s_np.album));

    /* Guess format from extension */
    const char *ext = strrchr(filename, '.');
    if (ext) {
        if (strcmp(ext, ".flac") == 0) {
            strncpy(s_np.format_name, "FLAC", sizeof(s_np.format_name));
            s_np.sample_rate = 96000;
            s_np.bits_per_sample = 24;
            s_np.is_dsd = false;
        } else if (strcmp(ext, ".dsf") == 0 || strcmp(ext, ".dff") == 0) {
            strncpy(s_np.format_name, "DSD64", sizeof(s_np.format_name));
            s_np.sample_rate = 2822400;
            s_np.bits_per_sample = 1;
            s_np.is_dsd = true;
        } else if (strcmp(ext, ".mp3") == 0) {
            strncpy(s_np.format_name, "MP3", sizeof(s_np.format_name));
            s_np.sample_rate = 44100;
            s_np.bits_per_sample = 16;
            s_np.is_dsd = false;
        } else {
            strncpy(s_np.format_name, "???", sizeof(s_np.format_name));
            s_np.sample_rate = 44100;
            s_np.bits_per_sample = 16;
            s_np.is_dsd = false;
        }
    }
    s_np.duration_ms  = 240000;   /* 4:00 */
    s_np.position_ms  = 0;
    s_np.gain_db      = 0.0f;
    s_np.state        = UI_PLAYBACK_PLAYING;
    s_np.source       = UI_SOURCE_SD;
}
