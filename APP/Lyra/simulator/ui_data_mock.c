/*
 * ui_data_mock.c — Mock data provider for the PC simulator.
 *
 * Returns static fake data so the UI can be developed and tested
 * without an ESP32-P4 or any real audio subsystem.
 */

#include "ui_data.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

/* User-defined EQ presets (mock storage) */
static ui_eq_user_preset_t s_user_presets[UI_MAX_USER_PRESETS];
static int s_user_preset_count = 0;

/* Playback queue (mock) */
static ui_queue_data_t s_queue = {
    .count = 0,
    .current_index = -1,
    .shuffle_enabled = false,
};

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
    /* Find in built-in presets */
    for (int i = 0; i < NUM_PRESETS; i++) {
        if (strcmp(name, s_preset_names[i]) == 0) {
            s_preset_idx = i;
            s_sys.dsp_preset = s_preset_names[i];
            s_sys.dsp_enabled = true;
            memcpy(s_sys.eq_bands, s_preset_curves[i], UI_EQ_BANDS);
            return;
        }
    }
    /* Find in user presets */
    for (int i = 0; i < s_user_preset_count; i++) {
        if (strcmp(name, s_user_presets[i].name) == 0) {
            s_sys.dsp_preset = s_user_presets[i].name;
            s_sys.dsp_enabled = true;
            memcpy(s_sys.eq_bands, s_user_presets[i].bands, UI_EQ_BANDS);
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

    /* Artist: parent folder name */
    const char *folder = strrchr(s_browser_path, '/');
    strncpy(s_np.artist, (folder && folder[1]) ? folder + 1 : "Unknown Artist",
            sizeof(s_np.artist) - 1);
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

    /* Populate queue from current directory (all audio files) */
    const ui_browser_entry_t *entries;
    int count;
    get_mock_listing(&entries, &count);

    s_queue.count = 0;
    s_queue.current_index = -1;

    /* Mock durations: vary per track */
    static const uint32_t mock_durations[] = {
        818000, 446000, 308000, 334000, 752000,
        312000, 480000, 266000, 415000
    };
    int ndur = (int)(sizeof(mock_durations) / sizeof(mock_durations[0]));

    for (int i = 0; i < count && s_queue.count < UI_QUEUE_MAX_ITEMS; i++) {
        if (entries[i].type != UI_ENTRY_FILE) continue;

        ui_queue_item_t *q = &s_queue.items[s_queue.count];

        /* Title: filename without extension */
        strncpy(q->title, entries[i].name, sizeof(q->title) - 1);
        q->title[sizeof(q->title) - 1] = '\0';
        char *qdot = strrchr(q->title, '.');
        if (qdot) *qdot = '\0';

        /* Artist: parent folder name */
        strncpy(q->artist, (folder && folder[1]) ? folder + 1 : "Unknown",
                sizeof(q->artist) - 1);

        /* Duration from mock table */
        q->duration_ms = mock_durations[s_queue.count % ndur];

        /* Track which one was clicked */
        if (strcmp(entries[i].name, filename) == 0) {
            s_queue.current_index = s_queue.count;
            s_np.duration_ms = q->duration_ms;
        }

        s_queue.count++;
    }
}

/* -----------------------------------------------------------------------
 * Mock Net Audio / Streaming data
 * ----------------------------------------------------------------------- */

static ui_net_audio_data_t s_net = {
    .state          = UI_NET_AUDIO_IDLE,
    .stream_title   = "",
    .stream_url     = "",
    .codec          = "",
    .bitrate_kbps   = 0,
    .sample_rate    = 0,
    .bits_per_sample = 0,
    .elapsed_ms     = 0,
    .error_msg      = "",
    .preset_count   = 0,
};

/* Simulated connection progress: counts getter calls after play command */
static int s_net_connect_ticks = 0;

static const ui_radio_preset_t s_radio_presets[] = {
    { "Radio Paradise",    "https://stream.radioparadise.com/flac",                    "flac" },
    { "Jazz24",            "https://live.wostreaming.net/direct/ppm-jazz24mp3-ibc1",   "mp3"  },
    { "KEXP Seattle",      "https://kexp-mp3-128.streamguys1.com/kexp128.mp3",         "mp3"  },
    { "Classical KING FM", "https://classicalking.streamguys1.com/king-fm-aac-128",    "aac"  },
    { "SomaFM Groove",    "https://ice2.somafm.com/groovesalad-256-mp3",               "mp3"  },
    { "NTS Radio 1",      "https://stream-relay-geo.ntslive.net/stream",               "mp3"  },
};
#define MOCK_PRESET_COUNT  (int)(sizeof(s_radio_presets) / sizeof(s_radio_presets[0]))

ui_net_audio_data_t ui_data_get_net_audio(void)
{
    /* Load presets on first call */
    if (s_net.preset_count == 0) {
        int count = MOCK_PRESET_COUNT;
        if (count > UI_NET_AUDIO_MAX_PRESETS) count = UI_NET_AUDIO_MAX_PRESETS;
        for (int i = 0; i < count; i++)
            s_net.presets[i] = s_radio_presets[i];
        s_net.preset_count = count;
    }

    /* Simulate connection state progression */
    if (s_net.state == UI_NET_AUDIO_CONNECTING) {
        s_net_connect_ticks++;
        if (s_net_connect_ticks >= 3) {
            s_net.state = UI_NET_AUDIO_BUFFERING;
            s_net_connect_ticks = 0;
        }
    } else if (s_net.state == UI_NET_AUDIO_BUFFERING) {
        s_net_connect_ticks++;
        if (s_net_connect_ticks >= 3) {
            s_net.state = UI_NET_AUDIO_PLAYING;
            s_net_connect_ticks = 0;
        }
    }

    /* Advance elapsed time while playing */
    if (s_net.state == UI_NET_AUDIO_PLAYING) {
        s_net.elapsed_ms += 200;

        /* Update now-playing with stream info */
        s_np.source = UI_SOURCE_NET;
        strncpy(s_np.title, s_net.stream_title, sizeof(s_np.title) - 1);
        strncpy(s_np.artist, "Internet Radio", sizeof(s_np.artist) - 1);
        strncpy(s_np.album, "", sizeof(s_np.album) - 1);
        strncpy(s_np.format_name, s_net.codec, sizeof(s_np.format_name) - 1);
        s_np.sample_rate = s_net.sample_rate;
        s_np.bits_per_sample = s_net.bits_per_sample;
        s_np.duration_ms = 0;   /* Live stream — unknown duration */
        s_np.position_ms = s_net.elapsed_ms;
        s_np.state = UI_PLAYBACK_PLAYING;
        s_np.is_dsd = false;
    }

    return s_net;
}

void ui_cmd_net_play(const char *url)
{
    printf("[MOCK] Net play: %s\n", url);

    strncpy(s_net.stream_url, url, sizeof(s_net.stream_url) - 1);
    s_net.stream_url[sizeof(s_net.stream_url) - 1] = '\0';
    s_net.state = UI_NET_AUDIO_CONNECTING;
    s_net.elapsed_ms = 0;
    s_net.error_msg[0] = '\0';
    s_net_connect_ticks = 0;

    /* Look up in presets for name + codec */
    bool found = false;
    for (int i = 0; i < MOCK_PRESET_COUNT; i++) {
        if (strcmp(url, s_radio_presets[i].url) == 0) {
            strncpy(s_net.stream_title, s_radio_presets[i].name,
                    sizeof(s_net.stream_title) - 1);
            /* Uppercase codec for display */
            const char *hint = s_radio_presets[i].codec_hint;
            if (strcmp(hint, "flac") == 0) {
                strncpy(s_net.codec, "FLAC", sizeof(s_net.codec));
                s_net.sample_rate = 44100;
                s_net.bits_per_sample = 16;
                s_net.bitrate_kbps = 900;
            } else if (strcmp(hint, "aac") == 0) {
                strncpy(s_net.codec, "AAC", sizeof(s_net.codec));
                s_net.sample_rate = 44100;
                s_net.bits_per_sample = 16;
                s_net.bitrate_kbps = 128;
            } else {
                strncpy(s_net.codec, "MP3", sizeof(s_net.codec));
                s_net.sample_rate = 44100;
                s_net.bits_per_sample = 16;
                s_net.bitrate_kbps = 320;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        strncpy(s_net.stream_title, "Custom Stream", sizeof(s_net.stream_title));
        strncpy(s_net.codec, "MP3", sizeof(s_net.codec));
        s_net.sample_rate = 44100;
        s_net.bits_per_sample = 16;
        s_net.bitrate_kbps = 128;
    }
}

void ui_cmd_net_stop(void)
{
    printf("[MOCK] Net stop\n");
    s_net.state = UI_NET_AUDIO_IDLE;
    s_net.stream_title[0] = '\0';
    s_net.stream_url[0] = '\0';
    s_net.codec[0] = '\0';
    s_net.bitrate_kbps = 0;
    s_net.sample_rate = 0;
    s_net.bits_per_sample = 0;
    s_net.elapsed_ms = 0;
    s_net.error_msg[0] = '\0';
    s_net_connect_ticks = 0;

    /* Return now-playing to SD source */
    s_np.source = UI_SOURCE_SD;
    strncpy(s_np.title, "Wish You Were Here", sizeof(s_np.title));
    strncpy(s_np.artist, "Pink Floyd", sizeof(s_np.artist));
    strncpy(s_np.album, "Wish You Were Here (Remastered)", sizeof(s_np.album));
    strncpy(s_np.format_name, "FLAC", sizeof(s_np.format_name));
    s_np.sample_rate = 96000;
    s_np.bits_per_sample = 24;
    s_np.duration_ms = 334000;
    s_np.position_ms = 67000;
    s_np.state = UI_PLAYBACK_PLAYING;
}

void ui_cmd_net_pause_resume(void)
{
    if (s_net.state == UI_NET_AUDIO_PLAYING) {
        printf("[MOCK] Net pause\n");
        s_net.state = UI_NET_AUDIO_PAUSED;
        s_np.state = UI_PLAYBACK_PAUSED;
    } else if (s_net.state == UI_NET_AUDIO_PAUSED) {
        printf("[MOCK] Net resume\n");
        s_net.state = UI_NET_AUDIO_PLAYING;
        s_np.state = UI_PLAYBACK_PLAYING;
    }
}

/* -----------------------------------------------------------------------
 * Mock WiFi scan data
 * ----------------------------------------------------------------------- */

static ui_wifi_scan_data_t s_wifi = {
    .state          = UI_WIFI_CONNECTED,
    .ap_count       = 0,
    .connected_ssid = "HomeNetwork",
    .connected_rssi = -52,
    .error_msg      = "",
};

static const ui_wifi_ap_t s_mock_aps[] = {
    { "HomeNetwork",   -52, 6,  UI_WIFI_AUTH_SECURED },
    { "CafeWiFi",      -71, 1,  UI_WIFI_AUTH_OPEN    },
    { "5G_Neighbor",   -85, 11, UI_WIFI_AUTH_SECURED },
    { "TP-Link_A7B2",  -63, 3,  UI_WIFI_AUTH_SECURED },
    { "FreeHotspot",   -78, 6,  UI_WIFI_AUTH_OPEN    },
    { "NETGEAR-5G",    -90, 36, UI_WIFI_AUTH_SECURED },
    { "Apartment_42",  -67, 9,  UI_WIFI_AUTH_SECURED },
    { "xfinitywifi",   -82, 1,  UI_WIFI_AUTH_OPEN    },
};
#define MOCK_AP_COUNT  8

ui_wifi_scan_data_t ui_data_get_wifi_scan(void)
{
    return s_wifi;
}

void ui_cmd_wifi_scan(void)
{
    printf("[MOCK] WiFi scan started\n");

    /* Populate results instantly */
    int count = MOCK_AP_COUNT;
    if (count > UI_WIFI_MAX_APS) count = UI_WIFI_MAX_APS;
    for (int i = 0; i < count; i++)
        s_wifi.aps[i] = s_mock_aps[i];
    s_wifi.ap_count = count;

    s_wifi.state = s_sys.wifi_connected ? UI_WIFI_CONNECTED : UI_WIFI_IDLE;
}

void ui_cmd_wifi_connect(const char *ssid, const char *password)
{
    printf("[MOCK] WiFi connect: ssid='%s' pass='%s'\n",
           ssid, password ? password : "(none)");

    s_sys.wifi_connected = true;
    strncpy(s_sys.wifi_ssid, ssid, sizeof(s_sys.wifi_ssid) - 1);
    s_sys.wifi_ssid[sizeof(s_sys.wifi_ssid) - 1] = '\0';

    /* Find AP in scan results for RSSI */
    s_sys.wifi_rssi = -60;
    for (int i = 0; i < s_wifi.ap_count; i++) {
        if (strcmp(s_wifi.aps[i].ssid, ssid) == 0) {
            s_sys.wifi_rssi = s_wifi.aps[i].rssi;
            break;
        }
    }

    strncpy(s_wifi.connected_ssid, ssid, sizeof(s_wifi.connected_ssid) - 1);
    s_wifi.connected_ssid[sizeof(s_wifi.connected_ssid) - 1] = '\0';
    s_wifi.connected_rssi = s_sys.wifi_rssi;
    s_wifi.state = UI_WIFI_CONNECTED;
}

void ui_cmd_wifi_disconnect(void)
{
    printf("[MOCK] WiFi disconnect\n");
    s_sys.wifi_connected = false;
    s_sys.wifi_ssid[0] = '\0';
    s_sys.wifi_rssi = 0;
    s_wifi.connected_ssid[0] = '\0';
    s_wifi.connected_rssi = 0;
    s_wifi.state = UI_WIFI_IDLE;
}

/* -----------------------------------------------------------------------
 * Mock USB DAC data
 * ----------------------------------------------------------------------- */

static ui_usb_dac_data_t s_usb_dac = {
    .state           = UI_USB_DAC_DISCONNECTED,
    .enabled         = false,
    .sample_rate     = 0,
    .bits_per_sample = 0,
    .is_dsd          = false,
    .dsd_rate        = 0,
    .format_str      = "",
    .level_db_l      = -100.0f,
    .level_db_r      = -100.0f,
};
static int s_usb_connect_ticks = 0;

ui_usb_dac_data_t ui_data_get_usb_dac(void)
{
    if (!s_usb_dac.enabled) return s_usb_dac;

    /* Simulate connection → streaming progression */
    if (s_usb_dac.state == UI_USB_DAC_CONNECTED) {
        s_usb_connect_ticks++;
        if (s_usb_connect_ticks >= 3) {
            s_usb_dac.state = UI_USB_DAC_STREAMING;
            s_usb_dac.sample_rate = 384000;
            s_usb_dac.bits_per_sample = 32;
            s_usb_dac.is_dsd = false;
            strncpy(s_usb_dac.format_str, "PCM 384 kHz / 32-bit",
                    sizeof(s_usb_dac.format_str) - 1);
            s_usb_connect_ticks = 0;
        }
    }

    /* Simulate fluctuating signal levels when streaming */
    if (s_usb_dac.state == UI_USB_DAC_STREAMING) {
        /* Pseudo-random level between -24 and -6 dBFS */
        s_usb_dac.level_db_l = -24.0f + (float)(rand() % 180) / 10.0f;
        s_usb_dac.level_db_r = -24.0f + (float)(rand() % 180) / 10.0f;

        /* Update now-playing to reflect USB source */
        s_np.source = UI_SOURCE_USB;
        strncpy(s_np.title, "USB Audio", sizeof(s_np.title) - 1);
        strncpy(s_np.artist, "External Source", sizeof(s_np.artist) - 1);
        s_np.album[0] = '\0';
        strncpy(s_np.format_name, "PCM", sizeof(s_np.format_name) - 1);
        s_np.sample_rate = s_usb_dac.sample_rate;
        s_np.bits_per_sample = s_usb_dac.bits_per_sample;
        s_np.duration_ms = 0;
        s_np.position_ms = 0;
        s_np.state = UI_PLAYBACK_PLAYING;
        s_np.is_dsd = s_usb_dac.is_dsd;
    }

    return s_usb_dac;
}

void ui_cmd_usb_dac_enable(bool enable)
{
    printf("[MOCK] USB DAC %s\n", enable ? "enabled" : "disabled");

    s_usb_dac.enabled = enable;

    if (enable) {
        s_usb_dac.state = UI_USB_DAC_CONNECTED;
        s_usb_connect_ticks = 0;
        s_usb_dac.level_db_l = -100.0f;
        s_usb_dac.level_db_r = -100.0f;
    } else {
        s_usb_dac.state = UI_USB_DAC_DISCONNECTED;
        s_usb_dac.sample_rate = 0;
        s_usb_dac.bits_per_sample = 0;
        s_usb_dac.is_dsd = false;
        s_usb_dac.dsd_rate = 0;
        s_usb_dac.format_str[0] = '\0';
        s_usb_dac.level_db_l = -100.0f;
        s_usb_dac.level_db_r = -100.0f;
        s_usb_connect_ticks = 0;

        /* Return now-playing to SD source */
        s_np.source = UI_SOURCE_SD;
        strncpy(s_np.title, "Wish You Were Here", sizeof(s_np.title));
        strncpy(s_np.artist, "Pink Floyd", sizeof(s_np.artist));
        strncpy(s_np.album, "Wish You Were Here (Remastered)", sizeof(s_np.album));
        strncpy(s_np.format_name, "FLAC", sizeof(s_np.format_name));
        s_np.sample_rate = 96000;
        s_np.bits_per_sample = 24;
        s_np.duration_ms = 334000;
        s_np.position_ms = 67000;
        s_np.state = UI_PLAYBACK_PLAYING;
    }
}

/* -----------------------------------------------------------------------
 * EQ preset commands
 * ----------------------------------------------------------------------- */

ui_eq_presets_data_t ui_data_get_eq_presets(void)
{
    ui_eq_presets_data_t data = {0};
    data.count = s_user_preset_count;
    for (int i = 0; i < s_user_preset_count; i++)
        data.presets[i] = s_user_presets[i];
    return data;
}

void ui_cmd_save_eq_preset(const char *name)
{
    printf("[MOCK] Save EQ preset: %s\n", name);

    /* Overwrite if name already exists */
    for (int i = 0; i < s_user_preset_count; i++) {
        if (strcmp(s_user_presets[i].name, name) == 0) {
            memcpy(s_user_presets[i].bands, s_sys.eq_bands, UI_EQ_BANDS);
            s_sys.dsp_preset = s_user_presets[i].name;
            return;
        }
    }

    /* Add new preset */
    if (s_user_preset_count >= UI_MAX_USER_PRESETS) return;
    strncpy(s_user_presets[s_user_preset_count].name, name, UI_PRESET_NAME_LEN - 1);
    s_user_presets[s_user_preset_count].name[UI_PRESET_NAME_LEN - 1] = '\0';
    memcpy(s_user_presets[s_user_preset_count].bands, s_sys.eq_bands, UI_EQ_BANDS);
    s_sys.dsp_preset = s_user_presets[s_user_preset_count].name;
    s_user_preset_count++;
}

void ui_cmd_delete_eq_preset(const char *name)
{
    printf("[MOCK] Delete EQ preset: %s\n", name);
    for (int i = 0; i < s_user_preset_count; i++) {
        if (strcmp(s_user_presets[i].name, name) == 0) {
            for (int j = i; j < s_user_preset_count - 1; j++)
                s_user_presets[j] = s_user_presets[j + 1];
            s_user_preset_count--;
            /* If deleted the active preset, switch to Custom */
            if (s_sys.dsp_preset && strcmp(s_sys.dsp_preset, name) == 0)
                s_sys.dsp_preset = "Custom";
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * Queue data + commands
 * ----------------------------------------------------------------------- */

ui_queue_data_t ui_data_get_queue(void)
{
    return s_queue;
}

void ui_cmd_queue_jump(int index)
{
    printf("[MOCK] Queue jump to %d\n", index);
    if (index < 0 || index >= s_queue.count) return;
    s_queue.current_index = index;

    /* Update now-playing from queue item */
    strncpy(s_np.title, s_queue.items[index].title, sizeof(s_np.title) - 1);
    strncpy(s_np.artist, s_queue.items[index].artist, sizeof(s_np.artist) - 1);
    s_np.duration_ms = s_queue.items[index].duration_ms;
    s_np.position_ms = 0;
    s_np.state = UI_PLAYBACK_PLAYING;
    s_np.source = UI_SOURCE_SD;
}

void ui_cmd_queue_remove(int index)
{
    printf("[MOCK] Queue remove %d\n", index);
    if (index < 0 || index >= s_queue.count) return;

    for (int i = index; i < s_queue.count - 1; i++)
        s_queue.items[i] = s_queue.items[i + 1];
    s_queue.count--;

    if (s_queue.current_index == index)
        s_queue.current_index = (s_queue.count > 0) ? index % s_queue.count : -1;
    else if (s_queue.current_index > index)
        s_queue.current_index--;
}

void ui_cmd_queue_clear(void)
{
    printf("[MOCK] Queue clear\n");
    s_queue.count = 0;
    s_queue.current_index = -1;
}

void ui_cmd_queue_shuffle_toggle(void)
{
    s_queue.shuffle_enabled = !s_queue.shuffle_enabled;
    printf("[MOCK] Shuffle %s\n", s_queue.shuffle_enabled ? "ON" : "OFF");
}

/* -----------------------------------------------------------------------
 * Device info getter
 * ----------------------------------------------------------------------- */

ui_device_info_t ui_data_get_device_info(void)
{
    ui_device_info_t info = {0};

    /* Device */
    strncpy(info.model, "Lyra DAP", sizeof(info.model));
    strncpy(info.dac, "ES9039Q2M", sizeof(info.dac));
    strncpy(info.display_res, "720 x 1280", sizeof(info.display_res));

    /* Firmware */
    strncpy(info.fw_version, "1.0.0-dev", sizeof(info.fw_version));
    strncpy(info.build_date, __DATE__, sizeof(info.build_date));
    strncpy(info.idf_version, "v5.5.2", sizeof(info.idf_version));

    /* Hardware */
    strncpy(info.cpu, "ESP32-P4 RISC-V 400MHz", sizeof(info.cpu));
    info.internal_ram_used_kb  = 312;
    info.internal_ram_total_kb = 768;
    info.psram_used_kb         = 8192;
    info.psram_total_kb        = 32768;
    info.flash_total_mb        = 32;

    /* Storage */
    strncpy(info.sd_capacity, "64 GB", sizeof(info.sd_capacity));
    strncpy(info.sd_used, "23.4 GB", sizeof(info.sd_used));
    strncpy(info.sd_free, "40.6 GB", sizeof(info.sd_free));
    strncpy(info.sd_filesystem, "exFAT", sizeof(info.sd_filesystem));
    snprintf(info.current_file, sizeof(info.current_file),
             "%s/%s", s_browser_path, s_np.title);

    /* Connectivity */
    strncpy(info.wifi_mac, "A4:CF:12:E8:3B:7D", sizeof(info.wifi_mac));
    strncpy(info.bt_mac, "A4:CF:12:E8:3B:7E", sizeof(info.bt_mac));
    if (s_sys.wifi_connected)
        strncpy(info.ip_address, "192.168.1.42", sizeof(info.ip_address));
    else
        strncpy(info.ip_address, "N/A", sizeof(info.ip_address));

    /* System */
    info.uptime_seconds = lv_tick_get() / 1000;
    info.cpu_temp_c     = 47;

    switch (s_np.source) {
        case UI_SOURCE_SD:        strncpy(info.audio_source, "SD Card", sizeof(info.audio_source)); break;
        case UI_SOURCE_USB:       strncpy(info.audio_source, "USB DAC", sizeof(info.audio_source)); break;
        case UI_SOURCE_NET:       strncpy(info.audio_source, "Net Audio", sizeof(info.audio_source)); break;
        case UI_SOURCE_BLUETOOTH: strncpy(info.audio_source, "Bluetooth", sizeof(info.audio_source)); break;
        default:                  strncpy(info.audio_source, "None", sizeof(info.audio_source)); break;
    }

    return info;
}

/* -----------------------------------------------------------------------
 * Mock Qobuz data
 * ----------------------------------------------------------------------- */

static const ui_qobuz_profile_t s_mock_qobuz_profiles[] = {
    { "q1", "Personal",  "audiophile@example.com",  true  },
    { "q2", "Family",    "family@example.com",      false },
};
#define MOCK_QOBUZ_PROFILE_COUNT  2

static ui_qobuz_data_t s_qobuz = {
    .auth_state = UI_QOBUZ_AUTH_IDLE,
    .username   = "",
    .error_msg  = "",
    .searching  = false,
    .view       = UI_QOBUZ_VIEW_PROFILES,
    .profile_count = 0,
    .editing_profile_idx = -1,
    .result_count = 0,
    .playing_track_id = "",
};

static const ui_qobuz_track_t s_mock_qobuz_results[] = {
    { "Shine On You Crazy Diamond (Pts. 1-5)", "Pink Floyd",
      "Wish You Were Here", "47683562", 812, 192000, 24, true },
    { "Welcome to the Machine", "Pink Floyd",
      "Wish You Were Here", "47683563", 450, 192000, 24, true },
    { "Have a Cigar", "Pink Floyd",
      "Wish You Were Here", "47683564", 307, 192000, 24, true },
    { "Wish You Were Here", "Pink Floyd",
      "Wish You Were Here", "47683565", 338, 192000, 24, true },
    { "Shine On You Crazy Diamond (Pts. 6-9)", "Pink Floyd",
      "Wish You Were Here", "47683566", 743, 192000, 24, true },
};
#define MOCK_QOBUZ_RESULT_COUNT  5

ui_qobuz_data_t ui_data_get_qobuz(void)
{
    /* Populate profiles on first call */
    if (s_qobuz.profile_count == 0) {
        int count = MOCK_QOBUZ_PROFILE_COUNT;
        if (count > UI_QOBUZ_MAX_PROFILES) count = UI_QOBUZ_MAX_PROFILES;
        for (int i = 0; i < count; i++)
            s_qobuz.profiles[i] = s_mock_qobuz_profiles[i];
        s_qobuz.profile_count = count;
    }
    return s_qobuz;
}

void ui_cmd_qobuz_login(const char *email, const char *password)
{
    printf("[MOCK] Qobuz login: %s\n", email);
    strncpy(s_qobuz.username, email, sizeof(s_qobuz.username) - 1);
    s_qobuz.username[sizeof(s_qobuz.username) - 1] = '\0';
    s_qobuz.auth_state = UI_QOBUZ_AUTH_OK;
    s_qobuz.error_msg[0] = '\0';
}

void ui_cmd_qobuz_search(const char *query)
{
    printf("[MOCK] Qobuz search: %s\n", query);
    /* Populate mock results on search */
    int count = MOCK_QOBUZ_RESULT_COUNT;
    if (count > UI_QOBUZ_MAX_RESULTS) count = UI_QOBUZ_MAX_RESULTS;
    for (int i = 0; i < count; i++)
        s_qobuz.results[i] = s_mock_qobuz_results[i];
    s_qobuz.result_count = count;
    s_qobuz.searching = false;
}

void ui_cmd_qobuz_save_profile(const char *id, const char *name,
                                const char *email, const char *pass)
{
    printf("[MOCK] Qobuz save profile: id=%s name=%s email=%s\n", id, name, email);
    (void)pass;
    /* Find existing by id */
    for (int i = 0; i < s_qobuz.profile_count; i++) {
        if (strcmp(s_qobuz.profiles[i].id, id) == 0) {
            strncpy(s_qobuz.profiles[i].name, name, sizeof(s_qobuz.profiles[i].name) - 1);
            strncpy(s_qobuz.profiles[i].email, email, sizeof(s_qobuz.profiles[i].email) - 1);
            s_qobuz.view = UI_QOBUZ_VIEW_PROFILES;
            s_qobuz.editing_profile_idx = -1;
            return;
        }
    }
    /* Add new */
    if (s_qobuz.profile_count >= UI_QOBUZ_MAX_PROFILES) return;
    int idx = s_qobuz.profile_count;
    strncpy(s_qobuz.profiles[idx].id, id, sizeof(s_qobuz.profiles[idx].id) - 1);
    strncpy(s_qobuz.profiles[idx].name, name, sizeof(s_qobuz.profiles[idx].name) - 1);
    strncpy(s_qobuz.profiles[idx].email, email, sizeof(s_qobuz.profiles[idx].email) - 1);
    s_qobuz.profiles[idx].connected = false;
    s_qobuz.profile_count++;
    s_qobuz.view = UI_QOBUZ_VIEW_PROFILES;
    s_qobuz.editing_profile_idx = -1;
}

void ui_cmd_qobuz_delete_profile(const char *id)
{
    printf("[MOCK] Qobuz delete profile: %s\n", id);
    for (int i = 0; i < s_qobuz.profile_count; i++) {
        if (strcmp(s_qobuz.profiles[i].id, id) == 0) {
            for (int j = i; j < s_qobuz.profile_count - 1; j++)
                s_qobuz.profiles[j] = s_qobuz.profiles[j + 1];
            s_qobuz.profile_count--;
            break;
        }
    }
    s_qobuz.view = UI_QOBUZ_VIEW_PROFILES;
    s_qobuz.editing_profile_idx = -1;
}

void ui_cmd_qobuz_select_profile(const char *id)
{
    printf("[MOCK] Qobuz select profile: %s\n", id);
    for (int i = 0; i < s_qobuz.profile_count; i++) {
        s_qobuz.profiles[i].connected = (strcmp(s_qobuz.profiles[i].id, id) == 0);
        if (s_qobuz.profiles[i].connected) {
            strncpy(s_qobuz.username, s_qobuz.profiles[i].email,
                    sizeof(s_qobuz.username) - 1);
            s_qobuz.auth_state = UI_QOBUZ_AUTH_OK;
        }
    }
    /* Populate results and switch to browse */
    int count = MOCK_QOBUZ_RESULT_COUNT;
    if (count > UI_QOBUZ_MAX_RESULTS) count = UI_QOBUZ_MAX_RESULTS;
    for (int i = 0; i < count; i++)
        s_qobuz.results[i] = s_mock_qobuz_results[i];
    s_qobuz.result_count = count;
    s_qobuz.view = UI_QOBUZ_VIEW_RESULTS;
}

void ui_cmd_qobuz_edit_profile(int index)
{
    printf("[MOCK] Qobuz edit profile: %d\n", index);
    s_qobuz.editing_profile_idx = index;
    s_qobuz.view = UI_QOBUZ_VIEW_PROFILE_EDIT;
}

void ui_cmd_qobuz_cancel_edit(void)
{
    printf("[MOCK] Qobuz cancel edit\n");
    s_qobuz.view = UI_QOBUZ_VIEW_PROFILES;
    s_qobuz.editing_profile_idx = -1;
}

void ui_cmd_qobuz_play_track(const char *track_id)
{
    printf("[MOCK] Qobuz play track: %s\n", track_id);
    strncpy(s_qobuz.playing_track_id, track_id,
            sizeof(s_qobuz.playing_track_id) - 1);
    s_qobuz.playing_track_id[sizeof(s_qobuz.playing_track_id) - 1] = '\0';

    /* Find the track and update now-playing */
    for (int i = 0; i < s_qobuz.result_count; i++) {
        if (strcmp(s_qobuz.results[i].track_id, track_id) == 0) {
            strncpy(s_np.title, s_qobuz.results[i].title, sizeof(s_np.title) - 1);
            strncpy(s_np.artist, s_qobuz.results[i].artist, sizeof(s_np.artist) - 1);
            strncpy(s_np.album, s_qobuz.results[i].album, sizeof(s_np.album) - 1);
            strncpy(s_np.format_name, "FLAC", sizeof(s_np.format_name));
            s_np.sample_rate = s_qobuz.results[i].sample_rate;
            s_np.bits_per_sample = s_qobuz.results[i].bits_per_sample;
            s_np.duration_ms = s_qobuz.results[i].duration_s * 1000;
            s_np.position_ms = 0;
            s_np.state = UI_PLAYBACK_PLAYING;
            s_np.source = UI_SOURCE_NET;
            s_np.is_dsd = false;
            break;
        }
    }
}

/* -----------------------------------------------------------------------
 * Mock Subsonic data
 * ----------------------------------------------------------------------- */

static const ui_subsonic_album_t s_mock_albums[] = {
    { "al-1", "Wish You Were Here",        "Pink Floyd",   1975, 5, 2640 },
    { "al-2", "Aja",                        "Steely Dan",   1977, 7, 2381 },
    { "al-3", "Symphony No. 9",             "Beethoven",    1824, 4, 3960 },
    { "al-4", "Brothers in Arms",           "Dire Straits", 1985, 9, 3360 },
    { "al-5", "Kind of Blue",               "Miles Davis",  1959, 5, 2760 },
};
#define MOCK_SUBSONIC_ALBUM_COUNT  5

static const ui_subsonic_track_t s_mock_tracks_album1[] = {
    { "tr-1", "Shine On You Crazy Diamond (Pts. 1-5)", "Pink Floyd",
      "Wish You Were Here", 812, 96000, 2400, 24, "flac", true },
    { "tr-2", "Welcome to the Machine", "Pink Floyd",
      "Wish You Were Here", 450, 96000, 2400, 24, "flac", true },
    { "tr-3", "Have a Cigar", "Pink Floyd",
      "Wish You Were Here", 307, 96000, 2400, 24, "flac", true },
    { "tr-4", "Wish You Were Here", "Pink Floyd",
      "Wish You Were Here", 338, 96000, 2400, 24, "flac", true },
    { "tr-5", "Shine On You Crazy Diamond (Pts. 6-9)", "Pink Floyd",
      "Wish You Were Here", 743, 96000, 2400, 24, "flac", true },
};
#define MOCK_TRACKS_ALBUM1_COUNT  5

static const ui_subsonic_track_t s_mock_tracks_album2[] = {
    { "tr-6",  "Black Cow",      "Steely Dan", "Aja", 312, 44100, 900, 16, "flac", false },
    { "tr-7",  "Aja",            "Steely Dan", "Aja", 480, 44100, 900, 16, "flac", false },
    { "tr-8",  "Deacon Blues",   "Steely Dan", "Aja", 452, 44100, 900, 16, "flac", false },
    { "tr-9",  "Peg",            "Steely Dan", "Aja", 237, 44100, 900, 16, "flac", false },
    { "tr-10", "Home at Last",   "Steely Dan", "Aja", 332, 44100, 900, 16, "flac", false },
    { "tr-11", "I Got the News", "Steely Dan", "Aja", 305, 44100, 900, 16, "flac", false },
    { "tr-12", "Josie",          "Steely Dan", "Aja", 263, 44100, 900, 16, "flac", false },
};
#define MOCK_TRACKS_ALBUM2_COUNT  7

static const ui_subsonic_profile_t s_mock_subsonic_profiles[] = {
    { "p1", "Home Navidrome",   "http://192.168.1.100:4533", "admin",   true  },
    { "p2", "Office Airsonic",  "http://music.office.com",   "john",    false },
    { "p3", "VPS Navidrome",    "https://music.example.com", "kelroy",  false },
};
#define MOCK_SUBSONIC_PROFILE_COUNT  3

static ui_subsonic_data_t s_subsonic = {
    .auth_state   = UI_SUBSONIC_AUTH_IDLE,
    .server_url   = "",
    .username     = "",
    .error_msg    = "",
    .loading      = false,
    .view         = UI_SUBSONIC_VIEW_PROFILES,
    .profile_count = 0,
    .editing_profile_idx = -1,
    .album_count  = 0,
    .track_count  = 0,
    .current_album_name = "",
    .playing_track_id   = "",
};

ui_subsonic_data_t ui_data_get_subsonic(void)
{
    /* Populate profiles on first call */
    if (s_subsonic.profile_count == 0) {
        int count = MOCK_SUBSONIC_PROFILE_COUNT;
        if (count > UI_SUBSONIC_MAX_PROFILES) count = UI_SUBSONIC_MAX_PROFILES;
        for (int i = 0; i < count; i++)
            s_subsonic.profiles[i] = s_mock_subsonic_profiles[i];
        s_subsonic.profile_count = count;
    }
    /* Populate albums when in albums view */
    if (s_subsonic.album_count == 0 &&
        s_subsonic.view == UI_SUBSONIC_VIEW_ALBUMS) {
        int count = MOCK_SUBSONIC_ALBUM_COUNT;
        if (count > UI_SUBSONIC_MAX_ITEMS) count = UI_SUBSONIC_MAX_ITEMS;
        for (int i = 0; i < count; i++)
            s_subsonic.albums[i] = s_mock_albums[i];
        s_subsonic.album_count = count;
    }
    return s_subsonic;
}

void ui_cmd_subsonic_connect(const char *url, const char *user, const char *pass)
{
    printf("[MOCK] Subsonic connect: %s user=%s\n", url, user);
    strncpy(s_subsonic.server_url, url, sizeof(s_subsonic.server_url) - 1);
    strncpy(s_subsonic.username, user, sizeof(s_subsonic.username) - 1);
    s_subsonic.auth_state = UI_SUBSONIC_AUTH_OK;
    s_subsonic.error_msg[0] = '\0';
}

void ui_cmd_subsonic_browse_albums(void)
{
    printf("[MOCK] Subsonic browse albums\n");
    s_subsonic.view = UI_SUBSONIC_VIEW_ALBUMS;
    s_subsonic.track_count = 0;
    s_subsonic.current_album_name[0] = '\0';
    /* Re-populate albums */
    int count = MOCK_SUBSONIC_ALBUM_COUNT;
    if (count > UI_SUBSONIC_MAX_ITEMS) count = UI_SUBSONIC_MAX_ITEMS;
    for (int i = 0; i < count; i++)
        s_subsonic.albums[i] = s_mock_albums[i];
    s_subsonic.album_count = count;
}

void ui_cmd_subsonic_open_album(const char *album_id)
{
    printf("[MOCK] Subsonic open album: %s\n", album_id);

    const ui_subsonic_track_t *tracks = NULL;
    int count = 0;
    const char *album_name = "";

    if (strcmp(album_id, "al-1") == 0) {
        tracks = s_mock_tracks_album1;
        count = MOCK_TRACKS_ALBUM1_COUNT;
        album_name = "Wish You Were Here";
    } else if (strcmp(album_id, "al-2") == 0) {
        tracks = s_mock_tracks_album2;
        count = MOCK_TRACKS_ALBUM2_COUNT;
        album_name = "Aja";
    } else {
        /* Default: show album1 tracks for any other album */
        tracks = s_mock_tracks_album1;
        count = MOCK_TRACKS_ALBUM1_COUNT;
        /* Find album name from mock list */
        for (int i = 0; i < MOCK_SUBSONIC_ALBUM_COUNT; i++) {
            if (strcmp(s_mock_albums[i].id, album_id) == 0) {
                album_name = s_mock_albums[i].name;
                break;
            }
        }
    }

    if (count > UI_SUBSONIC_MAX_ITEMS) count = UI_SUBSONIC_MAX_ITEMS;
    for (int i = 0; i < count; i++)
        s_subsonic.tracks[i] = tracks[i];
    s_subsonic.track_count = count;
    strncpy(s_subsonic.current_album_name, album_name,
            sizeof(s_subsonic.current_album_name) - 1);
    s_subsonic.view = UI_SUBSONIC_VIEW_TRACKS;
}

void ui_cmd_subsonic_search(const char *query)
{
    printf("[MOCK] Subsonic search: %s\n", query);
    /* Return a mix of tracks from both albums as search results */
    s_subsonic.view = UI_SUBSONIC_VIEW_SEARCH;
    int idx = 0;
    for (int i = 0; i < MOCK_TRACKS_ALBUM1_COUNT && idx < UI_SUBSONIC_MAX_ITEMS; i++)
        s_subsonic.tracks[idx++] = s_mock_tracks_album1[i];
    for (int i = 0; i < MOCK_TRACKS_ALBUM2_COUNT && idx < UI_SUBSONIC_MAX_ITEMS; i++)
        s_subsonic.tracks[idx++] = s_mock_tracks_album2[i];
    s_subsonic.track_count = idx;
    strncpy(s_subsonic.current_album_name, "Search Results",
            sizeof(s_subsonic.current_album_name) - 1);
}

void ui_cmd_subsonic_play_track(const char *track_id)
{
    printf("[MOCK] Subsonic play track: %s\n", track_id);
    strncpy(s_subsonic.playing_track_id, track_id,
            sizeof(s_subsonic.playing_track_id) - 1);

    /* Find the track and update now-playing */
    for (int i = 0; i < s_subsonic.track_count; i++) {
        if (strcmp(s_subsonic.tracks[i].id, track_id) == 0) {
            strncpy(s_np.title, s_subsonic.tracks[i].title, sizeof(s_np.title) - 1);
            strncpy(s_np.artist, s_subsonic.tracks[i].artist, sizeof(s_np.artist) - 1);
            strncpy(s_np.album, s_subsonic.tracks[i].album, sizeof(s_np.album) - 1);
            strncpy(s_np.format_name, "FLAC", sizeof(s_np.format_name));
            s_np.sample_rate = s_subsonic.tracks[i].sample_rate;
            s_np.bits_per_sample = s_subsonic.tracks[i].bits_per_sample;
            s_np.duration_ms = s_subsonic.tracks[i].duration_s * 1000;
            s_np.position_ms = 0;
            s_np.state = UI_PLAYBACK_PLAYING;
            s_np.source = UI_SOURCE_NET;
            s_np.is_dsd = false;
            break;
        }
    }
}

void ui_cmd_subsonic_back(void)
{
    printf("[MOCK] Subsonic back from view %d\n", s_subsonic.view);
    switch (s_subsonic.view) {
        case UI_SUBSONIC_VIEW_PROFILE_EDIT:
            s_subsonic.view = UI_SUBSONIC_VIEW_PROFILES;
            s_subsonic.editing_profile_idx = -1;
            break;
        case UI_SUBSONIC_VIEW_ALBUMS:
            s_subsonic.view = UI_SUBSONIC_VIEW_PROFILES;
            s_subsonic.auth_state = UI_SUBSONIC_AUTH_IDLE;
            break;
        case UI_SUBSONIC_VIEW_TRACKS:
        case UI_SUBSONIC_VIEW_SEARCH:
            ui_cmd_subsonic_browse_albums();
            break;
        case UI_SUBSONIC_VIEW_PROFILES:
        default:
            break;
    }
}

void ui_cmd_subsonic_save_profile(const char *id, const char *name,
                                   const char *url, const char *user,
                                   const char *pass)
{
    printf("[MOCK] Subsonic save profile: id=%s name=%s url=%s user=%s\n",
           id, name, url, user);
    (void)pass;
    /* Find existing by id */
    for (int i = 0; i < s_subsonic.profile_count; i++) {
        if (strcmp(s_subsonic.profiles[i].id, id) == 0) {
            strncpy(s_subsonic.profiles[i].name, name,
                    sizeof(s_subsonic.profiles[i].name) - 1);
            strncpy(s_subsonic.profiles[i].server_url, url,
                    sizeof(s_subsonic.profiles[i].server_url) - 1);
            strncpy(s_subsonic.profiles[i].username, user,
                    sizeof(s_subsonic.profiles[i].username) - 1);
            s_subsonic.view = UI_SUBSONIC_VIEW_PROFILES;
            s_subsonic.editing_profile_idx = -1;
            return;
        }
    }
    /* Add new */
    if (s_subsonic.profile_count >= UI_SUBSONIC_MAX_PROFILES) return;
    int idx = s_subsonic.profile_count;
    strncpy(s_subsonic.profiles[idx].id, id,
            sizeof(s_subsonic.profiles[idx].id) - 1);
    strncpy(s_subsonic.profiles[idx].name, name,
            sizeof(s_subsonic.profiles[idx].name) - 1);
    strncpy(s_subsonic.profiles[idx].server_url, url,
            sizeof(s_subsonic.profiles[idx].server_url) - 1);
    strncpy(s_subsonic.profiles[idx].username, user,
            sizeof(s_subsonic.profiles[idx].username) - 1);
    s_subsonic.profiles[idx].connected = false;
    s_subsonic.profile_count++;
    s_subsonic.view = UI_SUBSONIC_VIEW_PROFILES;
    s_subsonic.editing_profile_idx = -1;
}

void ui_cmd_subsonic_delete_profile(const char *id)
{
    printf("[MOCK] Subsonic delete profile: %s\n", id);
    for (int i = 0; i < s_subsonic.profile_count; i++) {
        if (strcmp(s_subsonic.profiles[i].id, id) == 0) {
            for (int j = i; j < s_subsonic.profile_count - 1; j++)
                s_subsonic.profiles[j] = s_subsonic.profiles[j + 1];
            s_subsonic.profile_count--;
            break;
        }
    }
    s_subsonic.view = UI_SUBSONIC_VIEW_PROFILES;
    s_subsonic.editing_profile_idx = -1;
}

void ui_cmd_subsonic_select_profile(const char *id)
{
    printf("[MOCK] Subsonic select profile: %s\n", id);
    for (int i = 0; i < s_subsonic.profile_count; i++) {
        s_subsonic.profiles[i].connected =
            (strcmp(s_subsonic.profiles[i].id, id) == 0);
        if (s_subsonic.profiles[i].connected) {
            strncpy(s_subsonic.server_url,
                    s_subsonic.profiles[i].server_url,
                    sizeof(s_subsonic.server_url) - 1);
            strncpy(s_subsonic.username,
                    s_subsonic.profiles[i].username,
                    sizeof(s_subsonic.username) - 1);
            s_subsonic.auth_state = UI_SUBSONIC_AUTH_OK;
        }
    }
    /* Switch to albums view */
    s_subsonic.view = UI_SUBSONIC_VIEW_ALBUMS;
    s_subsonic.album_count = 0;  /* Force repopulation */
}

void ui_cmd_subsonic_edit_profile(int index)
{
    printf("[MOCK] Subsonic edit profile: %d\n", index);
    s_subsonic.editing_profile_idx = index;
    s_subsonic.view = UI_SUBSONIC_VIEW_PROFILE_EDIT;
}

void ui_cmd_subsonic_cancel_edit(void)
{
    printf("[MOCK] Subsonic cancel edit\n");
    s_subsonic.view = UI_SUBSONIC_VIEW_PROFILES;
    s_subsonic.editing_profile_idx = -1;
}
