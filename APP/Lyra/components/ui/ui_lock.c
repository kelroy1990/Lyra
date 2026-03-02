/*
 * ui_lock.c -- Lock screen for Lyra (720x1280 portrait).
 *
 * Layout (top to bottom):
 *
 *   +----------------------------------+
 *   |  lock          WiFi   batt 82%   |  Status (48px)
 *   |                                  |
 *   |           Tuesday                |  Day (20pt, gray)
 *   |          10:35 AM                |  Clock (48pt, white)
 *   |       February 25, 2026          |  Date (14pt, gray)
 *   |                                  |
 *   |         +==============+         |
 *   |         |   AUDIO      |         |  Art (320x320)
 *   |         +==============+         |
 *   |                                  |
 *   |      Wish You Were Here          |  Title (20pt)
 *   |      Pink Floyd                  |  Artist (16pt, gray)
 *   |                                  |
 *   |         <<   >>   >>|            |  Transport (functional)
 *   |                                  |
 *   |          ^ Swipe up              |  Hint (breathing)
 *   +----------------------------------+
 */

#include "ui_internal.h"
#include <time.h>
#include <string.h>

/* Layout constants */
#define LOCK_ART_SIZE  320

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_lock;

/* Status */
static lv_obj_t *lbl_lock_wifi;
static lv_obj_t *lbl_lock_battery;
static lv_obj_t *bat_lock_fill;

/* Clock */
static lv_obj_t *lbl_lock_day;
static lv_obj_t *lbl_lock_time;
static lv_obj_t *lbl_lock_date;

/* Album art */
static lv_obj_t *art_lock_box;
static lv_obj_t *lbl_lock_art_text;

/* Track info */
static lv_obj_t *lbl_lock_title;
static lv_obj_t *lbl_lock_artist;

/* Transport */
static lv_obj_t *lock_transport;
static lv_obj_t *btn_lock_prev;
static lv_obj_t *btn_lock_next;
static lv_obj_t *lbl_lock_play_icon;

/* Current source (for play/pause callback routing) */
static ui_audio_source_t s_lock_source = UI_SOURCE_NONE;

/* Hint */
static lv_obj_t *lbl_unlock_hint;

/* -----------------------------------------------------------------------
 * Transport button press scale transition (same as Now Playing)
 * ----------------------------------------------------------------------- */

static const lv_style_prop_t s_trans_props[] = {
    LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0
};
static lv_style_transition_dsc_t s_trans_dsc;
static lv_style_t s_style_scale_pressed;
static bool s_styles_inited = false;

static void init_lock_styles(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    lv_style_transition_dsc_init(&s_trans_dsc, s_trans_props,
                                 lv_anim_path_ease_out, 100, 0, NULL);
    lv_style_init(&s_style_scale_pressed);
    lv_style_set_transform_scale_x(&s_style_scale_pressed, 230);
    lv_style_set_transform_scale_y(&s_style_scale_pressed, 230);
    lv_style_set_transition(&s_style_scale_pressed, &s_trans_dsc);
}

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_lock_play_pause(lv_event_t *e)
{
    (void)e;
    if (s_lock_source == UI_SOURCE_NET)
        ui_cmd_net_pause_resume();
    else
        ui_cmd_play_pause();
}

static void on_lock_prev(lv_event_t *e)
{
    (void)e;
    ui_cmd_prev();
}

static void on_lock_next(lv_event_t *e)
{
    (void)e;
    ui_cmd_next();
}

static void on_lock_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_TOP) {
        (void)e;
        ui_unlock();
    }
}

/* -----------------------------------------------------------------------
 * Hint breathing animation callback
 * ----------------------------------------------------------------------- */

static void anim_hint_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* -----------------------------------------------------------------------
 * Helper: create a transparent row
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

lv_obj_t *ui_lock_create(void)
{
    scr_lock = lv_obj_create(NULL);
    lv_obj_add_style(scr_lock, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_lock, LV_SCROLLBAR_MODE_OFF);

    /* Swipe-up gesture on the entire screen */
    lv_obj_add_event_cb(scr_lock, on_lock_gesture, LV_EVENT_GESTURE, NULL);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_lock);
    lv_obj_remove_style_all(main);
    lv_obj_set_size(main, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_pad_gap(main, 0, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * STATUS BAR (48px, subtle)
     * ================================================================== */
    lv_obj_t *bar = lv_obj_create(main);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, ui_theme_style_status_bar(), 0);
    lv_obj_set_size(bar, UI_HOR_RES, 48);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Left spacer (balances the right group for centering) */
    lv_obj_t *left_spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(left_spacer);
    lv_obj_set_size(left_spacer, 1, 1);

    /* Right group: WiFi + Battery */
    lv_obj_t *right_grp = lv_obj_create(bar);
    lv_obj_remove_style_all(right_grp);
    lv_obj_set_size(right_grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(right_grp, 12, 0);

    lbl_lock_wifi = lv_label_create(right_grp);
    lv_obj_add_style(lbl_lock_wifi, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_lock_wifi, LV_SYMBOL_WIFI);

    /* Battery visual indicator (bar + percentage) */
    lv_obj_t *bat_grp = lv_obj_create(right_grp);
    lv_obj_remove_style_all(bat_grp);
    lv_obj_set_size(bat_grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bat_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(bat_grp, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_gap(bat_grp, 0, 0);

    /* Battery body (outline) */
    lv_obj_t *bat_body = lv_obj_create(bat_grp);
    lv_obj_remove_style_all(bat_body);
    lv_obj_set_size(bat_body, 30, 14);
    lv_obj_set_style_radius(bat_body, 3, 0);
    lv_obj_set_style_border_width(bat_body, 2, 0);
    lv_obj_set_style_border_color(bat_body, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_pad_all(bat_body, 1, 0);
    lv_obj_set_scrollbar_mode(bat_body, LV_SCROLLBAR_MODE_OFF);

    /* Battery fill (width set in update) */
    bat_lock_fill = lv_obj_create(bat_body);
    lv_obj_remove_style_all(bat_lock_fill);
    lv_obj_set_height(bat_lock_fill, LV_PCT(100));
    lv_obj_set_width(bat_lock_fill, 24);
    lv_obj_set_style_radius(bat_lock_fill, 2, 0);
    lv_obj_set_style_bg_color(bat_lock_fill, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_bg_opa(bat_lock_fill, LV_OPA_COVER, 0);

    /* Battery tip (positive terminal nub) */
    lv_obj_t *bat_tip = lv_obj_create(bat_grp);
    lv_obj_remove_style_all(bat_tip);
    lv_obj_set_size(bat_tip, 3, 8);
    lv_obj_set_style_radius(bat_tip, 1, 0);
    lv_obj_set_style_bg_color(bat_tip, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_bg_opa(bat_tip, LV_OPA_COVER, 0);

    /* Percentage text */
    lbl_lock_battery = lv_label_create(bat_grp);
    lv_obj_add_style(lbl_lock_battery, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_lock_battery, "100%");
    lv_obj_set_style_margin_left(lbl_lock_battery, 6, 0);

    /* ==================================================================
     * CLOCK SECTION
     * ================================================================== */

    /* Day of week */
    lbl_lock_day = lv_label_create(main);
    lv_obj_add_style(lbl_lock_day, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_lock_day,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_lock_day, "---");
    lv_obj_set_style_text_align(lbl_lock_day, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_lock_day, 48, 0);

    /* Time (big) */
    lbl_lock_time = lv_label_create(main);
    lv_obj_set_style_text_font(lbl_lock_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_lock_time,
                                ui_theme_color_text_primary(), 0);
    lv_label_set_text(lbl_lock_time, "00:00");
    lv_obj_set_style_text_align(lbl_lock_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_lock_time, 4, 0);

    /* Full date */
    lbl_lock_date = lv_label_create(main);
    lv_obj_add_style(lbl_lock_date, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_lock_date,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_lock_date, "---");
    lv_obj_set_style_text_align(lbl_lock_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_lock_date, 4, 0);

    /* ==================================================================
     * ALBUM ART (320x320, smaller than Now Playing)
     * ================================================================== */
    art_lock_box = lv_obj_create(main);
    lv_obj_remove_style_all(art_lock_box);
    lv_obj_add_style(art_lock_box, ui_theme_style_art_placeholder(), 0);
    lv_obj_set_size(art_lock_box, LOCK_ART_SIZE, LOCK_ART_SIZE);
    lv_obj_set_style_margin_top(art_lock_box, 24, 0);

    lbl_lock_art_text = lv_label_create(art_lock_box);
    lv_obj_add_style(lbl_lock_art_text, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_font(lbl_lock_art_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_lock_art_text,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_lock_art_text, LV_SYMBOL_AUDIO);
    lv_obj_center(lbl_lock_art_text);

    /* ==================================================================
     * TRACK INFO
     * ================================================================== */
    lbl_lock_title = lv_label_create(main);
    lv_obj_add_style(lbl_lock_title, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_lock_title,
                                ui_theme_color_text_primary(), 0);
    lv_label_set_text(lbl_lock_title, "No Track");
    lv_label_set_long_mode(lbl_lock_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_lock_title, UI_HOR_RES - 80);
    lv_obj_set_style_text_align(lbl_lock_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_lock_title, 20, 0);

    lbl_lock_artist = lv_label_create(main);
    lv_obj_add_style(lbl_lock_artist, ui_theme_style_info(), 0);
    lv_obj_set_style_text_font(lbl_lock_artist, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_lock_artist,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_lock_artist, "");
    lv_obj_set_style_text_align(lbl_lock_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(lbl_lock_artist, 4, 0);

    /* ==================================================================
     * TRANSPORT CONTROLS (functional while locked)
     * ================================================================== */
    init_lock_styles();

    lock_transport = create_row(main, 320, 80);
    lv_obj_set_flex_align(lock_transport, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(lock_transport, 20, 0);

    /* Previous */
    btn_lock_prev = lv_button_create(lock_transport);
    lv_obj_add_style(btn_lock_prev, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_lock_prev, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_add_style(btn_lock_prev, &s_style_scale_pressed, LV_STATE_PRESSED);
    lv_obj_t *lbl_prev = lv_label_create(btn_lock_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_add_event_cb(btn_lock_prev, on_lock_prev, LV_EVENT_CLICKED, NULL);

    /* Play/Pause (golden circle) */
    lv_obj_t *btn_play = lv_button_create(lock_transport);
    lv_obj_add_style(btn_play, ui_theme_style_btn_play(), 0);
    lv_obj_add_style(btn_play, ui_theme_style_btn_play_pressed(),
                     LV_STATE_PRESSED);
    lv_obj_add_style(btn_play, &s_style_scale_pressed, LV_STATE_PRESSED);
    lbl_lock_play_icon = lv_label_create(btn_play);
    lv_label_set_text(lbl_lock_play_icon, LV_SYMBOL_PLAY);
    lv_obj_center(lbl_lock_play_icon);
    lv_obj_add_event_cb(btn_play, on_lock_play_pause, LV_EVENT_CLICKED, NULL);

    /* Next */
    btn_lock_next = lv_button_create(lock_transport);
    lv_obj_add_style(btn_lock_next, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_lock_next, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_add_style(btn_lock_next, &s_style_scale_pressed, LV_STATE_PRESSED);
    lv_obj_t *lbl_next = lv_label_create(btn_lock_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_add_event_cb(btn_lock_next, on_lock_next, LV_EVENT_CLICKED, NULL);

    /* ==================================================================
     * UNLOCK HINT (pushed to bottom with spacer)
     * ================================================================== */
    lv_obj_t *spacer = lv_obj_create(main);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lbl_unlock_hint = lv_label_create(main);
    lv_obj_add_style(lbl_unlock_hint, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_unlock_hint,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_unlock_hint, LV_SYMBOL_UP " Swipe up to unlock");
    lv_obj_set_style_text_align(lbl_unlock_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(lbl_unlock_hint, 40, 0);

    /* Breathing opacity animation on hint */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl_unlock_hint);
    lv_anim_set_values(&a, 80, 200);
    lv_anim_set_duration(&a, 1500);
    lv_anim_set_playback_duration(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_hint_opa_cb);
    lv_anim_start(&a);

    return scr_lock;
}

/* -----------------------------------------------------------------------
 * Update (called periodically while locked)
 * ----------------------------------------------------------------------- */

void ui_lock_update(const ui_now_playing_t *np,
                     const ui_system_status_t *sys)
{
    if (!scr_lock) return;

    /* -- Clock (real system time) -- */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t) {
        char buf[48];
        strftime(buf, sizeof(buf), "%A", t);        /* "Tuesday" */
        lv_label_set_text(lbl_lock_day, buf);

        strftime(buf, sizeof(buf), "%I:%M %p", t);  /* "10:35 AM" */
        lv_label_set_text(lbl_lock_time, buf);

        strftime(buf, sizeof(buf), "%B %d, %Y", t); /* "February 25, 2026" */
        lv_label_set_text(lbl_lock_date, buf);
    }

    /* -- Source-aware content -- */
    s_lock_source = np->source;

    switch (np->source) {
        case UI_SOURCE_USB: {
            ui_usb_dac_data_t dac = ui_data_get_usb_dac();
            lv_label_set_text(lbl_lock_art_text, LV_SYMBOL_USB);
            if (dac.state == UI_USB_DAC_STREAMING) {
                lv_label_set_text(lbl_lock_title, "USB DAC");
                lv_label_set_text(lbl_lock_artist, dac.format_str);
            } else {
                lv_label_set_text(lbl_lock_title, "USB DAC");
                lv_label_set_text(lbl_lock_artist, "Waiting for host...");
            }
            /* No transport — host controls playback */
            lv_obj_add_flag(lock_transport, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case UI_SOURCE_NET: {
            ui_net_audio_data_t na = ui_data_get_net_audio();
            lv_label_set_text(lbl_lock_art_text, LV_SYMBOL_WIFI);
            lv_label_set_text(lbl_lock_title,
                              na.stream_title[0] ? na.stream_title : "Net Audio");
            char net_info[64];
            if (na.bitrate_kbps > 0)
                lv_snprintf(net_info, sizeof(net_info), "%s  %lu kbps",
                            na.codec, (unsigned long)na.bitrate_kbps);
            else
                lv_snprintf(net_info, sizeof(net_info), "%s",
                            na.codec[0] ? na.codec : "Streaming");
            lv_label_set_text(lbl_lock_artist, net_info);
            /* Play/pause only — no prev/next for streams */
            lv_obj_clear_flag(lock_transport, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_lock_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_lock_next, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_lock_play_icon,
                              na.state == UI_NET_AUDIO_PLAYING ? LV_SYMBOL_PAUSE
                                                               : LV_SYMBOL_PLAY);
            break;
        }
        default: /* UI_SOURCE_SD, UI_SOURCE_BLUETOOTH, UI_SOURCE_NONE */
            lv_label_set_text(lbl_lock_art_text, LV_SYMBOL_AUDIO);
            lv_label_set_text(lbl_lock_title,
                              np->title[0] ? np->title : "No Track");
            lv_label_set_text(lbl_lock_artist, np->artist);
            /* Full transport */
            lv_obj_clear_flag(lock_transport, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(btn_lock_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(btn_lock_next, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_lock_play_icon,
                              np->state == UI_PLAYBACK_PLAYING ? LV_SYMBOL_PAUSE
                                                               : LV_SYMBOL_PLAY);
            break;
    }

    /* -- Battery (visual bar + colored percentage) -- */
    {
        lv_color_t bar_col, txt_col;
        if (sys->battery_charging) {
            bar_col = ui_theme_color_accent();
            txt_col = ui_theme_color_accent();
        } else if (sys->battery_pct > 50) {
            bar_col = lv_color_hex(0x4CAF50);          /* green */
            txt_col = ui_theme_color_text_primary();
        } else if (sys->battery_pct > 15) {
            bar_col = ui_theme_color_accent();          /* gold */
            txt_col = ui_theme_color_accent();
        } else {
            bar_col = lv_color_hex(0xE04040);           /* red */
            txt_col = lv_color_hex(0xE04040);
        }

        int32_t fill_w = (24 * sys->battery_pct) / 100;
        if (fill_w < 1) fill_w = 1;
        lv_obj_set_width(bat_lock_fill, fill_w);
        lv_obj_set_style_bg_color(bat_lock_fill, bar_col, 0);

        char bat_buf[24];
        if (sys->battery_charging)
            lv_snprintf(bat_buf, sizeof(bat_buf), LV_SYMBOL_CHARGE " %d%%",
                        sys->battery_pct);
        else
            lv_snprintf(bat_buf, sizeof(bat_buf), "%d%%", sys->battery_pct);
        lv_label_set_text(lbl_lock_battery, bat_buf);
        lv_obj_set_style_text_color(lbl_lock_battery, txt_col, 0);
    }

    /* -- WiFi (gold=connected, gray=disconnected) -- */
    lv_obj_set_style_text_color(lbl_lock_wifi,
                                sys->wifi_connected ? ui_theme_color_accent()
                                                    : ui_theme_color_text_secondary(),
                                0);
}
