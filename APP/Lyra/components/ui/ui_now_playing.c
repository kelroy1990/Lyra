/*
 * ui_now_playing.c — Now Playing screen for Lyra (720x1280 portrait).
 *
 * Layout (top to bottom, flex column with proper padding):
 *
 *   ┌──────────────────────────────────┐
 *   │  SD    Now Playing    WiFi  82%  │  Status bar (48px)
 *   │                                  │
 *   │        ┌──────────────┐          │
 *   │        │              │          │  Album art (480x480)
 *   │        │   ♪  AUDIO   │          │
 *   │        │              │          │
 *   │        └──────────────┘          │
 *   │                                  │
 *   │     Wish You Were Here           │  Title (28pt, white)
 *   │     Pink Floyd                   │  Artist (20pt, gray)
 *   │     Wish You Were Here           │  Album (14pt, dim)
 *   │                                  │
 *   │     FLAC · 96 kHz · 24-bit      │  Format (14pt, gold if hi-res)
 *   │                                  │
 *   │  1:07 ━━━━━━━━━━━░░░░░ 5:34     │  Progress slider
 *   │                                  │
 *   │        ⏮     ▶     ⏭           │  Transport (play = gold circle)
 *   │                                  │
 *   │  🔊 ━━━━━━━━━━━━━━━━━━ 72%      │  Volume slider
 *   │     DSP: Rock                    │  DSP status
 *   └──────────────────────────────────┘
 */

#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

/* Layout constants */
#define PAD_SIDE        40      /* Horizontal padding from screen edges  */
#define ART_SIZE        480     /* Album art square (67% of 720px width) */
#define CONTENT_W       (UI_HOR_RES - 2 * PAD_SIDE)  /* 640px usable */

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_now_playing;

/* Status bar */
static lv_obj_t *lbl_source;
static lv_obj_t *lbl_status_title;
static lv_obj_t *lbl_battery;
static lv_obj_t *lbl_wifi;

/* Album art */
static lv_obj_t *art_box;
static lv_obj_t *lbl_art_text;

/* Track info */
static lv_obj_t *lbl_track_title;
static lv_obj_t *lbl_artist;
static lv_obj_t *lbl_album;
static lv_obj_t *lbl_format_info;

/* Progress */
static lv_obj_t *slider_progress;
static lv_obj_t *lbl_time_elapsed;
static lv_obj_t *lbl_time_total;

/* Transport */
static lv_obj_t *btn_prev;
static lv_obj_t *btn_play;
static lv_obj_t *btn_next;
static lv_obj_t *lbl_play_icon;

/* Volume */
static lv_obj_t *slider_volume;
static lv_obj_t *lbl_vol_value;

/* DSP */
static lv_obj_t *lbl_dsp_status;

/* EQ visualizer */
static lv_obj_t *eq_bars[UI_EQ_BANDS];

/* Preset chips */
static lv_obj_t *preset_btns[4];
static const char *preset_names[] = { "Flat", "Rock", "Jazz", "Bass" };
#define NUM_PRESET_BTNS  4

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_play_pause(lv_event_t *e)
{
    (void)e;
    ui_cmd_play_pause();
}

static void on_prev(lv_event_t *e)
{
    (void)e;
    ui_cmd_prev();
}

static void on_next(lv_event_t *e)
{
    (void)e;
    ui_cmd_next();
}

static void on_progress_change(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    ui_cmd_seek((uint32_t)val);
}

static void on_volume_change(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    ui_cmd_set_volume((uint8_t)val);
}

static void on_preset_click(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    ui_cmd_set_dsp_preset(name);
}

/* -----------------------------------------------------------------------
 * Helper: create a transparent container (no bg, no border, no padding)
 * ----------------------------------------------------------------------- */

static lv_obj_t *create_row(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    return row;
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_now_playing_create(void)
{
    scr_now_playing = lv_obj_create(NULL);
    lv_obj_add_style(scr_now_playing, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_now_playing, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_now_playing);
    lv_obj_remove_style_all(main);
    lv_obj_set_size(main, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(main, 0, 0);
    lv_obj_set_style_pad_bottom(main, 20, 0);
    lv_obj_set_style_pad_gap(main, 0, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * STATUS BAR (48px)
     * ================================================================== */
    lv_obj_t *bar = lv_obj_create(main);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, ui_theme_style_status_bar(), 0);
    lv_obj_set_size(bar, UI_HOR_RES, 48);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Source badge (left) */
    lbl_source = lv_label_create(bar);
    lv_obj_add_style(lbl_source, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_source, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_source, "SD");

    /* Screen title (center, flex-grow) */
    lbl_status_title = lv_label_create(bar);
    lv_obj_add_style(lbl_status_title, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_status_title,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_status_title, "Now Playing");
    lv_obj_set_flex_grow(lbl_status_title, 1);
    lv_obj_set_style_text_align(lbl_status_title, LV_TEXT_ALIGN_CENTER, 0);

    /* Right group: WiFi + Battery */
    lv_obj_t *right_grp = lv_obj_create(bar);
    lv_obj_remove_style_all(right_grp);
    lv_obj_set_size(right_grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(right_grp, 12, 0);

    lbl_wifi = lv_label_create(right_grp);
    lv_obj_add_style(lbl_wifi, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);

    lbl_battery = lv_label_create(right_grp);
    lv_obj_add_style(lbl_battery, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_battery, LV_SYMBOL_BATTERY_FULL " 100%");

    /* ==================================================================
     * ALBUM ART (480x480, centered)
     * ================================================================== */
    art_box = lv_obj_create(main);
    lv_obj_remove_style_all(art_box);
    lv_obj_add_style(art_box, ui_theme_style_art_placeholder(), 0);
    lv_obj_set_size(art_box, ART_SIZE, ART_SIZE);
    lv_obj_set_style_margin_top(art_box, 32, 0);

    lbl_art_text = lv_label_create(art_box);
    lv_obj_add_style(lbl_art_text, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_font(lbl_art_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_art_text, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_art_text, LV_SYMBOL_AUDIO);
    lv_obj_center(lbl_art_text);

    /* ==================================================================
     * TRACK METADATA
     * ================================================================== */

    /* Title */
    lbl_track_title = lv_label_create(main);
    lv_obj_add_style(lbl_track_title, ui_theme_style_title(), 0);
    lv_label_set_text(lbl_track_title, "No Track");
    lv_label_set_long_mode(lbl_track_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_track_title, CONTENT_W);
    lv_obj_set_style_text_align(lbl_track_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_track_title, 28, 0);

    /* Artist */
    lbl_artist = lv_label_create(main);
    lv_obj_add_style(lbl_artist, ui_theme_style_subtitle(), 0);
    lv_label_set_text(lbl_artist, "");
    lv_label_set_long_mode(lbl_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_artist, CONTENT_W);
    lv_obj_set_style_text_align(lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_artist, 4, 0);

    /* Album */
    lbl_album = lv_label_create(main);
    lv_obj_add_style(lbl_album, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_album, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_album, "");
    lv_obj_set_width(lbl_album, CONTENT_W);
    lv_obj_set_style_text_align(lbl_album, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_album, 2, 0);

    /* Format info (gold for hi-res, dim gray for standard) */
    lbl_format_info = lv_label_create(main);
    lv_obj_add_style(lbl_format_info, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_format_info, "");
    lv_obj_set_style_text_align(lbl_format_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_format_info, 14, 0);

    /* ==================================================================
     * PROGRESS SECTION
     * ================================================================== */
    lv_obj_t *progress_row = create_row(main, CONTENT_W, 24);
    lv_obj_set_flex_align(progress_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(progress_row, 24, 0);
    lv_obj_set_style_pad_gap(progress_row, 12, 0);

    lbl_time_elapsed = lv_label_create(progress_row);
    lv_obj_add_style(lbl_time_elapsed, ui_theme_style_time(), 0);
    lv_label_set_text(lbl_time_elapsed, "0:00");
    lv_obj_set_width(lbl_time_elapsed, 52);

    slider_progress = lv_slider_create(progress_row);
    lv_obj_set_flex_grow(slider_progress, 1);
    lv_obj_set_height(slider_progress, 8);
    lv_slider_set_range(slider_progress, 0, 1000);
    lv_slider_set_value(slider_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_progress, ui_theme_color_slider_track(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider_progress, 4, LV_PART_MAIN);
    lv_obj_add_style(slider_progress, ui_theme_style_slider_main(),
                     LV_PART_INDICATOR);
    lv_obj_add_style(slider_progress, ui_theme_style_slider_knob(),
                     LV_PART_KNOB);
    lv_obj_add_event_cb(slider_progress, on_progress_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lbl_time_total = lv_label_create(progress_row);
    lv_obj_add_style(lbl_time_total, ui_theme_style_time(), 0);
    lv_label_set_text(lbl_time_total, "0:00");
    lv_obj_set_width(lbl_time_total, 52);
    lv_obj_set_style_text_align(lbl_time_total, LV_TEXT_ALIGN_RIGHT, 0);

    /* ==================================================================
     * TRANSPORT CONTROLS
     * ================================================================== */
    lv_obj_t *transport = create_row(main, 320, 80);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(transport, 16, 0);

    /* Previous button */
    btn_prev = lv_button_create(transport);
    lv_obj_add_style(btn_prev, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_prev, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_add_event_cb(btn_prev, on_prev, LV_EVENT_CLICKED, NULL);

    /* Play/Pause button — big golden circle */
    btn_play = lv_button_create(transport);
    lv_obj_add_style(btn_play, ui_theme_style_btn_play(), 0);
    lv_obj_add_style(btn_play, ui_theme_style_btn_play_pressed(),
                     LV_STATE_PRESSED);
    lbl_play_icon = lv_label_create(btn_play);
    lv_label_set_text(lbl_play_icon, LV_SYMBOL_PLAY);
    lv_obj_center(lbl_play_icon);
    lv_obj_add_event_cb(btn_play, on_play_pause, LV_EVENT_CLICKED, NULL);

    /* Next button */
    btn_next = lv_button_create(transport);
    lv_obj_add_style(btn_next, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_next, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_t *lbl_next_icon = lv_label_create(btn_next);
    lv_label_set_text(lbl_next_icon, LV_SYMBOL_NEXT);
    lv_obj_add_event_cb(btn_next, on_next, LV_EVENT_CLICKED, NULL);

    /* ==================================================================
     * VOLUME SECTION
     * ================================================================== */
    lv_obj_t *vol_row = create_row(main, CONTENT_W, 32);
    lv_obj_set_style_margin_top(vol_row, 24, 0);
    lv_obj_set_style_pad_gap(vol_row, 12, 0);

    lv_obj_t *lbl_vol_icon = lv_label_create(vol_row);
    lv_obj_add_style(lbl_vol_icon, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_vol_icon,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_vol_icon, LV_SYMBOL_VOLUME_MAX);

    slider_volume = lv_slider_create(vol_row);
    lv_obj_set_flex_grow(slider_volume, 1);
    lv_obj_set_height(slider_volume, 6);
    lv_slider_set_range(slider_volume, 0, 100);
    lv_slider_set_value(slider_volume, 75, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_volume, ui_theme_color_slider_track(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_volume, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider_volume, 3, LV_PART_MAIN);
    lv_obj_add_style(slider_volume, ui_theme_style_slider_main(),
                     LV_PART_INDICATOR);
    lv_obj_add_style(slider_volume, ui_theme_style_slider_knob(),
                     LV_PART_KNOB);
    lv_obj_add_event_cb(slider_volume, on_volume_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lbl_vol_value = lv_label_create(vol_row);
    lv_obj_add_style(lbl_vol_value, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_vol_value,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_vol_value, "75%");
    lv_obj_set_width(lbl_vol_value, 48);

    /* ==================================================================
     * DSP STATUS
     * ================================================================== */
    lbl_dsp_status = lv_label_create(main);
    lv_obj_add_style(lbl_dsp_status, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_dsp_status, "DSP: Flat");
    lv_obj_set_style_text_align(lbl_dsp_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_dsp_status, 12, 0);

    /* ==================================================================
     * EQ VISUALIZER (5-band bars + frequency labels)
     * ================================================================== */
    static const char *eq_freq_labels[UI_EQ_BANDS] = {
        "60", "250", "1k", "4k", "16k"
    };

    /* Container for the whole EQ section */
    lv_obj_t *eq_section = lv_obj_create(main);
    lv_obj_remove_style_all(eq_section);
    lv_obj_set_size(eq_section, CONTENT_W, 110);
    lv_obj_set_style_margin_top(eq_section, 8, 0);
    lv_obj_set_flex_flow(eq_section, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_section, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(eq_section, LV_SCROLLBAR_MODE_OFF);

    /* Each band: a column with track bg, colored bar, and freq label */
    for (int i = 0; i < UI_EQ_BANDS; i++) {
        /* Column container per band */
        lv_obj_t *band_col = lv_obj_create(eq_section);
        lv_obj_remove_style_all(band_col);
        lv_obj_set_size(band_col, 56, 110);
        lv_obj_set_flex_flow(band_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(band_col, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(band_col, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_gap(band_col, 4, 0);

        /* Track background (max height bar area) */
        lv_obj_t *bar_track = lv_obj_create(band_col);
        lv_obj_remove_style_all(bar_track);
        lv_obj_add_style(bar_track, ui_theme_style_eq_bar_track(), 0);
        lv_obj_set_size(bar_track, 20, 80);
        lv_obj_set_style_radius(bar_track, 3, 0);

        /* Active bar overlay (variable height, anchored to bottom) */
        eq_bars[i] = lv_obj_create(bar_track);
        lv_obj_remove_style_all(eq_bars[i]);
        lv_obj_add_style(eq_bars[i], ui_theme_style_eq_bar(), 0);
        lv_obj_set_width(eq_bars[i], 20);
        lv_obj_set_height(eq_bars[i], 40);   /* default 50% */
        lv_obj_align(eq_bars[i], LV_ALIGN_BOTTOM_MID, 0, 0);

        /* Frequency label below the bar */
        lv_obj_t *freq_lbl = lv_label_create(band_col);
        lv_obj_add_style(freq_lbl, ui_theme_style_info(), 0);
        lv_label_set_text(freq_lbl, eq_freq_labels[i]);
        lv_obj_set_style_text_align(freq_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* ==================================================================
     * PRESET SELECTOR CHIPS
     * ================================================================== */
    lv_obj_t *preset_row = create_row(main, CONTENT_W, 40);
    lv_obj_set_style_margin_top(preset_row, 8, 0);
    lv_obj_set_style_pad_gap(preset_row, 10, 0);

    for (int i = 0; i < NUM_PRESET_BTNS; i++) {
        preset_btns[i] = lv_button_create(preset_row);
        lv_obj_add_style(preset_btns[i], ui_theme_style_preset_chip(), 0);
        lv_obj_add_style(preset_btns[i], ui_theme_style_preset_chip_active(),
                         LV_STATE_CHECKED);
        lv_obj_add_flag(preset_btns[i], LV_OBJ_FLAG_CHECKABLE);

        lv_obj_t *lbl = lv_label_create(preset_btns[i]);
        lv_label_set_text(lbl, preset_names[i]);

        lv_obj_add_event_cb(preset_btns[i], on_preset_click,
                            LV_EVENT_CLICKED, (void *)preset_names[i]);
    }

    /* ==================================================================
     * BOTTOM NAVIGATION BAR
     * ================================================================== */
    /* Push nav bar to bottom with flex grow on a spacer */
    lv_obj_t *nav_spacer = lv_obj_create(main);
    lv_obj_remove_style_all(nav_spacer);
    lv_obj_set_size(nav_spacer, 1, 1);
    lv_obj_set_flex_grow(nav_spacer, 1);

    ui_create_nav_bar(main, UI_SCREEN_NOW_PLAYING);

    return scr_now_playing;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh data)
 * ----------------------------------------------------------------------- */

void ui_now_playing_update(const ui_now_playing_t *np,
                            const ui_system_status_t *sys)
{
    if (!scr_now_playing) return;

    /* -- Source badge (gold text) -- */
    const char *src_txt = "---";
    switch (np->source) {
        case UI_SOURCE_USB:       src_txt = "USB";  break;
        case UI_SOURCE_SD:        src_txt = LV_SYMBOL_SD_CARD " SD";  break;
        case UI_SOURCE_NET:       src_txt = LV_SYMBOL_WIFI " NET";    break;
        case UI_SOURCE_BLUETOOTH: src_txt = LV_SYMBOL_BLUETOOTH " BT"; break;
        default: break;
    }
    lv_label_set_text(lbl_source, src_txt);

    /* -- Battery (icon + percentage) -- */
    char bat_buf[24];
    const char *bat_icon;
    if (sys->battery_charging) {
        bat_icon = LV_SYMBOL_CHARGE;
    } else if (sys->battery_pct > 75) {
        bat_icon = LV_SYMBOL_BATTERY_FULL;
    } else if (sys->battery_pct > 50) {
        bat_icon = LV_SYMBOL_BATTERY_3;
    } else if (sys->battery_pct > 25) {
        bat_icon = LV_SYMBOL_BATTERY_2;
    } else if (sys->battery_pct > 10) {
        bat_icon = LV_SYMBOL_BATTERY_1;
    } else {
        bat_icon = LV_SYMBOL_BATTERY_EMPTY;
    }
    lv_snprintf(bat_buf, sizeof(bat_buf), "%s %d%%", bat_icon,
                sys->battery_pct);
    lv_label_set_text(lbl_battery, bat_buf);

    /* -- WiFi -- */
    if (sys->wifi_connected) {
        lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
        lv_obj_clear_flag(lbl_wifi, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_wifi, LV_OBJ_FLAG_HIDDEN);
    }

    /* -- Track info -- */
    lv_label_set_text(lbl_track_title, np->title[0] ? np->title : "No Track");
    lv_label_set_text(lbl_artist, np->artist);
    lv_label_set_text(lbl_album, np->album);

    /* -- Format info line (gold for hi-res content) -- */
    char fmt_buf[64];
    bool is_hires = false;

    if (np->is_dsd) {
        lv_snprintf(fmt_buf, sizeof(fmt_buf), "%s", np->format_name);
        is_hires = true;
    } else if (np->sample_rate > 0) {
        uint32_t sr_k = np->sample_rate / 1000;
        uint32_t sr_frac = (np->sample_rate % 1000) / 100;
        if (sr_frac)
            lv_snprintf(fmt_buf, sizeof(fmt_buf),
                        "%s \xe2\x80\xa2 %lu.%lu kHz \xe2\x80\xa2 %d-bit",
                        np->format_name, (unsigned long)sr_k,
                        (unsigned long)sr_frac, np->bits_per_sample);
        else
            lv_snprintf(fmt_buf, sizeof(fmt_buf),
                        "%s \xe2\x80\xa2 %lu kHz \xe2\x80\xa2 %d-bit",
                        np->format_name, (unsigned long)sr_k,
                        np->bits_per_sample);

        /* Hi-res = sample rate > 48kHz or bit depth > 16 */
        is_hires = (np->sample_rate > 48000 || np->bits_per_sample > 16);
    } else {
        fmt_buf[0] = '\0';
    }
    lv_label_set_text(lbl_format_info, fmt_buf);

    /* Color the format line: gold for hi-res, dim gray for standard */
    if (is_hires) {
        lv_obj_set_style_text_color(lbl_format_info,
                                    ui_theme_color_accent(), 0);
    } else {
        lv_obj_set_style_text_color(lbl_format_info,
                                    ui_theme_color_text_secondary(), 0);
    }

    /* -- Progress bar -- */
    if (np->duration_ms > 0) {
        lv_slider_set_range(slider_progress, 0, (int32_t)np->duration_ms);
        lv_slider_set_value(slider_progress, (int32_t)np->position_ms,
                            LV_ANIM_OFF);
    }

    char t_buf[16];
    ui_fmt_time(t_buf, sizeof(t_buf), np->position_ms);
    lv_label_set_text(lbl_time_elapsed, t_buf);

    ui_fmt_time(t_buf, sizeof(t_buf), np->duration_ms);
    lv_label_set_text(lbl_time_total, t_buf);

    /* -- Play/Pause icon -- */
    lv_label_set_text(lbl_play_icon,
                      np->state == UI_PLAYBACK_PLAYING ? LV_SYMBOL_PAUSE
                                                        : LV_SYMBOL_PLAY);

    /* -- Volume -- */
    lv_slider_set_value(slider_volume, sys->volume, LV_ANIM_OFF);
    char vol_buf[8];
    lv_snprintf(vol_buf, sizeof(vol_buf), "%d%%", sys->volume);
    lv_label_set_text(lbl_vol_value, vol_buf);

    /* -- DSP -- */
    char dsp_buf[48];
    if (sys->dsp_enabled && sys->dsp_preset)
        lv_snprintf(dsp_buf, sizeof(dsp_buf), "DSP: %s", sys->dsp_preset);
    else
        lv_snprintf(dsp_buf, sizeof(dsp_buf), "DSP: Off");
    lv_label_set_text(lbl_dsp_status, dsp_buf);

    /* -- EQ bars -- */
    for (int i = 0; i < UI_EQ_BANDS; i++) {
        /* Map gain (-12..+12 dB) to bar height (4..80 px).
         * Center (0 dB) = 40px.  +12 dB = 80px.  -12 dB = 4px min. */
        int32_t gain = sys->eq_bands[i];
        if (gain < -12) gain = -12;
        if (gain >  12) gain =  12;
        int32_t h = 40 + (gain * 80) / 24;  /* 40 ± 40 → range 0..80 */
        if (h < 4) h = 4;
        lv_obj_set_height(eq_bars[i], h);
        lv_obj_align(eq_bars[i], LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    /* -- Preset chips (highlight active via CHECKED state) -- */
    for (int i = 0; i < NUM_PRESET_BTNS; i++) {
        bool active = sys->dsp_enabled && sys->dsp_preset &&
                      strcmp(sys->dsp_preset, preset_names[i]) == 0;
        if (active)
            lv_obj_add_state(preset_btns[i], LV_STATE_CHECKED);
        else
            lv_obj_clear_state(preset_btns[i], LV_STATE_CHECKED);
    }
}
