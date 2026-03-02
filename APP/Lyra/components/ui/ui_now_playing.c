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
static lv_obj_t *bat_fill_np;
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
static lv_obj_t *progress_row;
static lv_obj_t *slider_progress;
static lv_obj_t *lbl_time_elapsed;
static lv_obj_t *lbl_time_total;

/* Transport */
static lv_obj_t *transport_row;
static lv_obj_t *btn_prev;
static lv_obj_t *btn_play;
static lv_obj_t *btn_next;
static lv_obj_t *lbl_play_icon;

/* Volume */
static lv_obj_t *slider_volume;
static lv_obj_t *lbl_vol_value;

/* DSP */
static lv_obj_t *lbl_dsp_status;

/* Queue button */
static lv_obj_t *btn_queue;

/* EQ visualizer */
static lv_obj_t *eq_bars[UI_EQ_BANDS];

/* Preset chips */
static lv_obj_t *preset_btns[4];
static const char *preset_names[] = { "Flat", "Rock", "Jazz", "Bass" };
#define NUM_PRESET_BTNS  4

/* -----------------------------------------------------------------------
 * EQ bar animation callback
 * ----------------------------------------------------------------------- */

static void anim_eq_height_cb(void *obj, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)obj, v);
    lv_obj_align((lv_obj_t *)obj, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* -----------------------------------------------------------------------
 * Transport button press scale transition
 * ----------------------------------------------------------------------- */

static const lv_style_prop_t s_trans_scale_props[] = {
    LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0
};
static lv_style_transition_dsc_t s_trans_scale;
static lv_style_t s_style_btn_scale_pressed;
static bool s_scale_styles_inited = false;

static void init_scale_styles(void)
{
    if (s_scale_styles_inited) return;
    s_scale_styles_inited = true;

    lv_style_transition_dsc_init(&s_trans_scale, s_trans_scale_props,
                                 lv_anim_path_ease_out, 100, 0, NULL);

    lv_style_init(&s_style_btn_scale_pressed);
    lv_style_set_transform_scale_x(&s_style_btn_scale_pressed, 230);  /* 90% of 256 */
    lv_style_set_transform_scale_y(&s_style_btn_scale_pressed, 230);
    lv_style_set_transition(&s_style_btn_scale_pressed, &s_trans_scale);
}

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

static void on_eq_tap(lv_event_t *e)
{
    (void)e;
    ui_navigate_to(UI_SCREEN_EQ);
}

static void on_queue_tap(lv_event_t *e)
{
    (void)e;
    ui_navigate_to(UI_SCREEN_QUEUE);
}

/* -----------------------------------------------------------------------
 * Source picker overlay
 * ----------------------------------------------------------------------- */

static lv_obj_t *s_source_overlay = NULL;

static void source_picker_dismiss(void)
{
    if (s_source_overlay) {
        lv_obj_delete(s_source_overlay);
        s_source_overlay = NULL;
    }
}

static void on_source_backdrop(lv_event_t *e)
{
    /* Only dismiss if the backdrop itself was clicked (not a child) */
    if (lv_event_get_target(e) == lv_event_get_current_target(e))
        source_picker_dismiss();
}

static void on_source_sd(lv_event_t *e)
{
    (void)e;
    source_picker_dismiss();
    /* If streaming, stop; otherwise just stay on SD */
    ui_cmd_net_stop();
    ui_cmd_usb_dac_enable(false);
}

static void on_source_net(lv_event_t *e)
{
    (void)e;
    source_picker_dismiss();
    ui_navigate_to(UI_SCREEN_NET_AUDIO);
}

static void on_source_usb(lv_event_t *e)
{
    (void)e;
    source_picker_dismiss();
    ui_navigate_to(UI_SCREEN_USB_DAC);
}

static void on_source_qobuz(lv_event_t *e)
{
    (void)e;
    source_picker_dismiss();
    ui_navigate_to(UI_SCREEN_QOBUZ);
}

static void on_source_subsonic(lv_event_t *e)
{
    (void)e;
    source_picker_dismiss();
    ui_navigate_to(UI_SCREEN_SUBSONIC);
}

/* Helper: create a clickable row in the source picker card */
static lv_obj_t *create_source_row(lv_obj_t *parent, const char *icon,
                                    const char *label, const char *right_text,
                                    lv_color_t right_col,
                                    lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 14, 0);
    lv_obj_set_style_pad_gap(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    /* Pressed feedback */
    lv_obj_set_style_bg_color(row, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

    /* Icon */
    lv_obj_t *lbl_icon = lv_label_create(row);
    lv_obj_add_style(lbl_icon, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_icon, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_icon, icon);

    /* Label (flex grow) */
    lv_obj_t *lbl_name = lv_label_create(row);
    lv_obj_add_style(lbl_name, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_name, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_name, label);
    lv_obj_set_flex_grow(lbl_name, 1);

    /* Right text (check mark or chevron) */
    lv_obj_t *lbl_right = lv_label_create(row);
    lv_obj_add_style(lbl_right, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_right, right_col, 0);
    lv_label_set_text(lbl_right, right_text);

    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

static void source_picker_show(ui_audio_source_t current_source)
{
    if (s_source_overlay) return;  /* Already open */

    lv_obj_t *layer = lv_layer_top();

    /* Semi-transparent backdrop */
    s_source_overlay = lv_obj_create(layer);
    lv_obj_remove_style_all(s_source_overlay);
    lv_obj_set_size(s_source_overlay, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_style_bg_color(s_source_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_source_overlay, (lv_opa_t)102, 0); /* ~40% */
    lv_obj_add_flag(s_source_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_source_overlay, on_source_backdrop,
                        LV_EVENT_CLICKED, NULL);

    /* Dropdown card */
    lv_obj_t *card = lv_obj_create(s_source_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 320, LV_SIZE_CONTENT);
    lv_obj_set_pos(card, 16, 52);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x252525), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_style_pad_hor(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 20, 0);
    lv_obj_set_style_shadow_opa(card, (lv_opa_t)80, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card, 0, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    /* Section header */
    lv_obj_t *lbl_header = lv_label_create(card);
    lv_obj_add_style(lbl_header, ui_theme_style_section_header(), 0);
    lv_label_set_text(lbl_header, "AUDIO SOURCE");
    lv_obj_set_style_pad_left(lbl_header, 16, 0);
    lv_obj_set_style_pad_top(lbl_header, 8, 0);
    lv_obj_set_style_pad_bottom(lbl_header, 4, 0);

    /* Check mark for active, chevron for others */
    const char *check = LV_SYMBOL_OK;
    lv_color_t col_active = lv_color_hex(0x4CAF50);
    lv_color_t col_nav = ui_theme_color_text_secondary();

    /* SD Card row */
    bool sd_active = (current_source == UI_SOURCE_SD ||
                      current_source == UI_SOURCE_NONE);
    create_source_row(card, LV_SYMBOL_SD_CARD, "SD Card",
                      sd_active ? check : LV_SYMBOL_RIGHT,
                      sd_active ? col_active : col_nav,
                      on_source_sd);

    /* Divider */
    lv_obj_t *div1 = lv_obj_create(card);
    lv_obj_remove_style_all(div1);
    lv_obj_set_size(div1, 288, 1);
    lv_obj_set_style_bg_color(div1, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_opa(div1, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(div1, 16, 0);

    /* Net Audio row */
    bool net_active = (current_source == UI_SOURCE_NET);
    create_source_row(card, LV_SYMBOL_WIFI, "Net Audio",
                      net_active ? check : LV_SYMBOL_RIGHT,
                      net_active ? col_active : col_nav,
                      on_source_net);

    /* Divider */
    lv_obj_t *div2 = lv_obj_create(card);
    lv_obj_remove_style_all(div2);
    lv_obj_set_size(div2, 288, 1);
    lv_obj_set_style_bg_color(div2, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(div2, 16, 0);

    /* USB DAC row */
    bool usb_active = (current_source == UI_SOURCE_USB);
    create_source_row(card, LV_SYMBOL_USB, "USB DAC",
                      usb_active ? check : LV_SYMBOL_RIGHT,
                      usb_active ? col_active : col_nav,
                      on_source_usb);

    /* Divider */
    lv_obj_t *div3 = lv_obj_create(card);
    lv_obj_remove_style_all(div3);
    lv_obj_set_size(div3, 288, 1);
    lv_obj_set_style_bg_color(div3, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_opa(div3, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(div3, 16, 0);

    /* Qobuz Hi-Fi row */
    create_source_row(card, LV_SYMBOL_AUDIO, "Qobuz Hi-Fi",
                      LV_SYMBOL_RIGHT, col_nav,
                      on_source_qobuz);

    /* Divider */
    lv_obj_t *div4 = lv_obj_create(card);
    lv_obj_remove_style_all(div4);
    lv_obj_set_size(div4, 288, 1);
    lv_obj_set_style_bg_color(div4, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_opa(div4, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(div4, 16, 0);

    /* Subsonic row */
    create_source_row(card, LV_SYMBOL_WIFI, "Subsonic",
                      LV_SYMBOL_RIGHT, col_nav,
                      on_source_subsonic);
}

/* Track current source for the picker */
static ui_audio_source_t s_current_source = UI_SOURCE_SD;

static void on_source_badge_click(lv_event_t *e)
{
    (void)e;
    source_picker_show(s_current_source);
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

    /* Source badge (left, tappable pill → source picker) */
    lbl_source = lv_label_create(bar);
    lv_obj_add_style(lbl_source, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_source, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_source, LV_SYMBOL_SD_CARD " SD " LV_SYMBOL_DOWN);
    lv_obj_add_flag(lbl_source, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_source, 10);
    /* Pill styling: subtle bg + thin border + rounded corners */
    lv_obj_set_style_bg_color(lbl_source, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(lbl_source, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(lbl_source, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(lbl_source, 1, 0);
    lv_obj_set_style_radius(lbl_source, 14, 0);
    lv_obj_set_style_pad_hor(lbl_source, 12, 0);
    lv_obj_set_style_pad_ver(lbl_source, 5, 0);
    /* Pressed feedback */
    lv_obj_set_style_bg_color(lbl_source, lv_color_hex(0x252525), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(lbl_source, ui_theme_color_accent(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(lbl_source, on_source_badge_click,
                        LV_EVENT_CLICKED, NULL);

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
    bat_fill_np = lv_obj_create(bat_body);
    lv_obj_remove_style_all(bat_fill_np);
    lv_obj_set_height(bat_fill_np, LV_PCT(100));
    lv_obj_set_width(bat_fill_np, 24);
    lv_obj_set_style_radius(bat_fill_np, 2, 0);
    lv_obj_set_style_bg_color(bat_fill_np, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_bg_opa(bat_fill_np, LV_OPA_COVER, 0);

    /* Battery tip (positive terminal nub) */
    lv_obj_t *bat_tip = lv_obj_create(bat_grp);
    lv_obj_remove_style_all(bat_tip);
    lv_obj_set_size(bat_tip, 3, 8);
    lv_obj_set_style_radius(bat_tip, 1, 0);
    lv_obj_set_style_bg_color(bat_tip, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_bg_opa(bat_tip, LV_OPA_COVER, 0);

    /* Percentage text */
    lbl_battery = lv_label_create(bat_grp);
    lv_obj_add_style(lbl_battery, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_battery, "100%");
    lv_obj_set_style_margin_left(lbl_battery, 6, 0);

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
    progress_row = create_row(main, CONTENT_W, 24);
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
    transport_row = create_row(main, 320, 80);
    lv_obj_set_flex_align(transport_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(transport_row, 16, 0);

    /* Init scale press styles (once) */
    init_scale_styles();

    /* Previous button */
    btn_prev = lv_button_create(transport_row);
    lv_obj_add_style(btn_prev, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_prev, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_add_style(btn_prev, &s_style_btn_scale_pressed, LV_STATE_PRESSED);
    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_add_event_cb(btn_prev, on_prev, LV_EVENT_CLICKED, NULL);

    /* Play/Pause button — big golden circle */
    btn_play = lv_button_create(transport_row);
    lv_obj_add_style(btn_play, ui_theme_style_btn_play(), 0);
    lv_obj_add_style(btn_play, ui_theme_style_btn_play_pressed(),
                     LV_STATE_PRESSED);
    lv_obj_add_style(btn_play, &s_style_btn_scale_pressed, LV_STATE_PRESSED);
    lbl_play_icon = lv_label_create(btn_play);
    lv_label_set_text(lbl_play_icon, LV_SYMBOL_PLAY);
    lv_obj_center(lbl_play_icon);
    lv_obj_add_event_cb(btn_play, on_play_pause, LV_EVENT_CLICKED, NULL);

    /* Next button */
    btn_next = lv_button_create(transport_row);
    lv_obj_add_style(btn_next, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_next, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_add_style(btn_next, &s_style_btn_scale_pressed, LV_STATE_PRESSED);
    lv_obj_t *lbl_next_icon = lv_label_create(btn_next);
    lv_label_set_text(lbl_next_icon, LV_SYMBOL_NEXT);
    lv_obj_add_event_cb(btn_next, on_next, LV_EVENT_CLICKED, NULL);

    /* ==================================================================
     * QUEUE BUTTON (between transport and volume)
     * ================================================================== */
    btn_queue = lv_button_create(main);
    lv_obj_add_style(btn_queue, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_queue, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn_queue, 16, 0);
    lv_obj_set_style_pad_ver(btn_queue, 6, 0);
    lv_obj_set_style_margin_top(btn_queue, 8, 0);
    lv_obj_t *lbl_queue = lv_label_create(btn_queue);
    lv_obj_add_style(lbl_queue, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_queue, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_queue, LV_SYMBOL_LIST " Queue");
    lv_obj_add_event_cb(btn_queue, on_queue_tap, LV_EVENT_CLICKED, NULL);
    /* Initially hidden (shown when queue has items) */
    lv_obj_add_flag(btn_queue, LV_OBJ_FLAG_HIDDEN);

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
     * EQ CARD (tappable card: header + bars + presets → opens EQ screen)
     * ================================================================== */
    static const char *eq_freq_labels[UI_EQ_BANDS] = {
        "60", "250", "1k", "4k", "16k"
    };

    lv_obj_t *eq_card = lv_obj_create(main);
    lv_obj_remove_style_all(eq_card);
    lv_obj_set_size(eq_card, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(eq_card, 12, 0);
    /* Card background */
    lv_obj_set_style_bg_color(eq_card, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(eq_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eq_card, 16, 0);
    lv_obj_set_style_pad_all(eq_card, 16, 0);
    lv_obj_set_style_pad_gap(eq_card, 8, 0);
    lv_obj_set_flex_flow(eq_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(eq_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(eq_card, LV_SCROLLBAR_MODE_OFF);
    /* Tappable with pressed feedback */
    lv_obj_add_flag(eq_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(eq_card, lv_color_hex(0x252525), LV_STATE_PRESSED);
    lv_obj_add_event_cb(eq_card, on_eq_tap, LV_EVENT_CLICKED, NULL);

    /* Card header row: DSP status (left) + "Equalizer >" (right) */
    lv_obj_t *eq_header = lv_obj_create(eq_card);
    lv_obj_remove_style_all(eq_header);
    lv_obj_set_size(eq_header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(eq_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(eq_header, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(eq_header, LV_OBJ_FLAG_CLICKABLE);

    lbl_dsp_status = lv_label_create(eq_header);
    lv_obj_add_style(lbl_dsp_status, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_dsp_status, "DSP: Flat");

    lv_obj_t *lbl_eq_nav = lv_label_create(eq_header);
    lv_obj_add_style(lbl_eq_nav, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_eq_nav, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_eq_nav, "Equalizer " LV_SYMBOL_RIGHT);

    /* EQ bars area (inside card) */
    lv_obj_t *eq_section = lv_obj_create(eq_card);
    lv_obj_remove_style_all(eq_section);
    lv_obj_set_size(eq_section, lv_pct(100), 100);
    lv_obj_set_flex_flow(eq_section, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_section, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(eq_section, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(eq_section, LV_OBJ_FLAG_CLICKABLE);

    /* Each band: a column with track bg, colored bar, and freq label */
    for (int i = 0; i < UI_EQ_BANDS; i++) {
        /* Column container per band */
        lv_obj_t *band_col = lv_obj_create(eq_section);
        lv_obj_remove_style_all(band_col);
        lv_obj_set_size(band_col, 56, 100);
        lv_obj_set_flex_flow(band_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(band_col, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(band_col, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_gap(band_col, 4, 0);
        lv_obj_clear_flag(band_col, LV_OBJ_FLAG_CLICKABLE);

        /* Track background (max height bar area) */
        lv_obj_t *bar_track = lv_obj_create(band_col);
        lv_obj_remove_style_all(bar_track);
        lv_obj_add_style(bar_track, ui_theme_style_eq_bar_track(), 0);
        lv_obj_set_size(bar_track, 20, 72);
        lv_obj_set_style_radius(bar_track, 3, 0);
        lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_CLICKABLE);

        /* Active bar overlay (variable height, anchored to bottom) */
        eq_bars[i] = lv_obj_create(bar_track);
        lv_obj_remove_style_all(eq_bars[i]);
        lv_obj_add_style(eq_bars[i], ui_theme_style_eq_bar(), 0);
        lv_obj_set_width(eq_bars[i], 20);
        lv_obj_set_height(eq_bars[i], 36);   /* default 50% */
        lv_obj_align(eq_bars[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(eq_bars[i], LV_OBJ_FLAG_CLICKABLE);

        /* Frequency label below the bar */
        lv_obj_t *freq_lbl = lv_label_create(band_col);
        lv_obj_add_style(freq_lbl, ui_theme_style_info(), 0);
        lv_label_set_text(freq_lbl, eq_freq_labels[i]);
        lv_obj_set_style_text_align(freq_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* Preset chips row (inside card) */
    lv_obj_t *preset_row = lv_obj_create(eq_card);
    lv_obj_remove_style_all(preset_row);
    lv_obj_set_size(preset_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(preset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preset_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(preset_row, 10, 0);
    lv_obj_set_scrollbar_mode(preset_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(preset_row, LV_OBJ_FLAG_CLICKABLE);

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

    /* -- Source badge (gold pill with dropdown arrow) -- */
    const char *src_txt = "---";
    switch (np->source) {
        case UI_SOURCE_USB:       src_txt = LV_SYMBOL_USB " USB " LV_SYMBOL_DOWN;        break;
        case UI_SOURCE_SD:        src_txt = LV_SYMBOL_SD_CARD " SD " LV_SYMBOL_DOWN;     break;
        case UI_SOURCE_NET:       src_txt = LV_SYMBOL_WIFI " NET " LV_SYMBOL_DOWN;       break;
        case UI_SOURCE_BLUETOOTH: src_txt = LV_SYMBOL_BLUETOOTH " BT " LV_SYMBOL_DOWN;   break;
        default: break;
    }
    lv_label_set_text(lbl_source, src_txt);

    /* Track current source for picker */
    s_current_source = np->source;

    /* -- Source-adaptive layout -- */
    bool is_sd  = (np->source == UI_SOURCE_SD || np->source == UI_SOURCE_NONE);
    bool is_net = (np->source == UI_SOURCE_NET);
    bool is_usb = (np->source == UI_SOURCE_USB);

    /* Art icon adapts to source */
    if (is_net)
        lv_label_set_text(lbl_art_text, LV_SYMBOL_AUDIO "\nRADIO");
    else if (is_usb)
        lv_label_set_text(lbl_art_text, LV_SYMBOL_USB "\nUSB DAC");
    else
        lv_label_set_text(lbl_art_text, LV_SYMBOL_AUDIO);

    /* Progress row: hide for USB, show LIVE for NET */
    if (is_usb) {
        lv_obj_add_flag(progress_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(progress_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* Transport: hide all for USB, hide prev/next for NET */
    if (is_usb) {
        lv_obj_add_flag(transport_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(transport_row, LV_OBJ_FLAG_HIDDEN);
        if (is_net) {
            lv_obj_add_flag(btn_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_next, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(btn_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(btn_next, LV_OBJ_FLAG_HIDDEN);
        }
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

        /* Fill width: max 24px (30 - 2×border - 2×pad) */
        int32_t fill_w = (24 * sys->battery_pct) / 100;
        if (fill_w < 1) fill_w = 1;
        lv_obj_set_width(bat_fill_np, fill_w);
        lv_obj_set_style_bg_color(bat_fill_np, bar_col, 0);

        char bat_buf[24];
        if (sys->battery_charging)
            lv_snprintf(bat_buf, sizeof(bat_buf), LV_SYMBOL_CHARGE " %d%%",
                        sys->battery_pct);
        else
            lv_snprintf(bat_buf, sizeof(bat_buf), "%d%%", sys->battery_pct);
        lv_label_set_text(lbl_battery, bat_buf);
        lv_obj_set_style_text_color(lbl_battery, txt_col, 0);
    }

    /* -- WiFi (gold=connected, gray=disconnected) -- */
    lv_obj_set_style_text_color(lbl_wifi,
                                sys->wifi_connected ? ui_theme_color_accent()
                                                    : ui_theme_color_text_secondary(), 0);

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
    if (is_net) {
        /* Live stream: show elapsed, "LIVE" for total, disable seek */
        char t_buf[16];
        ui_fmt_time(t_buf, sizeof(t_buf), np->position_ms);
        lv_label_set_text(lbl_time_elapsed, t_buf);
        lv_label_set_text(lbl_time_total, "LIVE");
        lv_slider_set_range(slider_progress, 0, 1000);
        lv_slider_set_value(slider_progress, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(slider_progress, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(slider_progress, LV_OBJ_FLAG_CLICKABLE);
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
    }

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

    /* -- EQ bars (smooth height animation) -- */
    for (int i = 0; i < UI_EQ_BANDS; i++) {
        /* Map gain (-12..+12 dB) to bar height (4..72 px).
         * Center (0 dB) = 36px.  +12 dB = 72px.  -12 dB = 4px min. */
        int32_t gain = sys->eq_bands[i];
        if (gain < -12) gain = -12;
        if (gain >  12) gain =  12;
        int32_t h = 36 + (gain * 3);  /* 36 ± 36 → range 0..72 */
        if (h < 4) h = 4;

        int32_t cur_h = lv_obj_get_height(eq_bars[i]);
        if (cur_h != h) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, eq_bars[i]);
            lv_anim_set_values(&a, cur_h, h);
            lv_anim_set_duration(&a, 200);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, anim_eq_height_cb);
            lv_anim_start(&a);
        }
    }

    /* -- Queue button visibility -- */
    {
        ui_queue_data_t q = ui_data_get_queue();
        if (q.count > 0)
            lv_obj_clear_flag(btn_queue, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(btn_queue, LV_OBJ_FLAG_HIDDEN);
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
