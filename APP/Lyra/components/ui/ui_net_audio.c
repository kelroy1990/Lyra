/*
 * ui_net_audio.c — Net Audio / Streaming sub-screen for Lyra.
 *
 * Layout (top to bottom):
 *
 *   ┌──────────────────────────────────┐
 *   │  ◀  Net Audio                    │  Header (48px)
 *   ├──────────────────────────────────┤
 *   │  NOW STREAMING                   │  (hidden when idle)
 *   │  ┌────────────────────────────┐  │
 *   │  │  Radio Paradise            │  │  Stream title
 *   │  │  FLAC · 44.1 kHz · 16-bit │  │  Format line
 *   │  │  ● Playing · 2:34         │  │  State + elapsed
 *   │  │  [⏸ Pause]   [⏹ Stop]   │  │  Controls
 *   │  └────────────────────────────┘  │
 *   │                                  │
 *   │  PLAY URL                        │
 *   │  ┌────────────────────────────┐  │
 *   │  │ https://                   │  │  URL textarea
 *   │  └────────────────────────────┘  │
 *   │  [▶ Play URL]                    │
 *   │                                  │
 *   │  RADIO STATIONS                  │
 *   │  ─────────────────────────────   │
 *   │  📻 Radio Paradise     FLAC     │
 *   │  ─────────────────────────────   │
 *   │  📻 Jazz24              MP3     │
 *   │          ... (scrollable) ...    │
 *   │                                  │
 *   │  [====== KEYBOARD ======]        │  (shown on URL tap)
 *   └──────────────────────────────────┘
 */

#include "ui_internal.h"
#include <string.h>

/* Layout constants */
#define PAD_SIDE  24
#define CONTENT_W (UI_HOR_RES - 2 * PAD_SIDE)

/* State colors */
#define COL_STATE_PLAYING   lv_color_hex(0x4CAF50)   /* green  */
#define COL_STATE_BUFFERING lv_color_hex(0xFFA726)   /* amber  */
#define COL_STATE_PAUSED    lv_color_hex(0x999999)   /* gray   */
#define COL_STATE_ERROR     lv_color_hex(0xFF5252)   /* red    */

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_net_audio;

/* Streaming card */
static lv_obj_t *lbl_stream_section;   /* "NOW STREAMING" section header */
static lv_obj_t *card_stream;
static lv_obj_t *lbl_stream_title;
static lv_obj_t *lbl_stream_format;
static lv_obj_t *dot_stream_state;
static lv_obj_t *lbl_stream_state;
static lv_obj_t *btn_pause;
static lv_obj_t *lbl_pause_icon;
static lv_obj_t *btn_stop;
static lv_obj_t *div_stream;           /* divider below stream section */

/* URL input */
static lv_obj_t *ta_url;
static lv_obj_t *btn_play_url;
static lv_obj_t *kb_url;

/* Preset list container */
static lv_obj_t *preset_list;

/* Toast state tracking */
static ui_net_audio_state_t s_prev_na_state = UI_NET_AUDIO_IDLE;
static bool s_toast_seeded = false;

/* Preset rebuild tracking */
static int s_last_preset_count = -1;

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    /* Hide keyboard if visible */
    if (kb_url) lv_obj_add_flag(kb_url, LV_OBJ_FLAG_HIDDEN);
    ui_navigate_to(UI_SCREEN_NOW_PLAYING);
}

static void on_pause_resume(lv_event_t *e)
{
    (void)e;
    ui_cmd_net_pause_resume();
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    ui_cmd_net_stop();
}

static void on_play_url(lv_event_t *e)
{
    (void)e;
    const char *url = lv_textarea_get_text(ta_url);
    if (url && url[0]) {
        ui_cmd_net_play(url);
        /* Hide keyboard */
        if (kb_url) lv_obj_add_flag(kb_url, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Preset click — URL stored as user_data */
static void on_preset_click(lv_event_t *e)
{
    const char *url = (const char *)lv_event_get_user_data(e);
    if (url) ui_cmd_net_play(url);
}

/* Keyboard events */
static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        lv_obj_add_flag(kb_url, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_ta_focus(lv_event_t *e)
{
    (void)e;
    if (kb_url) {
        lv_keyboard_set_textarea(kb_url, ta_url);
        lv_obj_clear_flag(kb_url, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_ta_defocus(lv_event_t *e)
{
    (void)e;
    if (kb_url) lv_obj_add_flag(kb_url, LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Helpers: create section header + divider (reuse settings pattern)
 * ----------------------------------------------------------------------- */

static lv_obj_t *create_section(lv_obj_t *parent, const char *text,
                                int32_t margin_top)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_add_style(lbl, ui_theme_style_section_header(), 0);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, UI_HOR_RES);
    lv_obj_set_style_margin_top(lbl, margin_top, 0);
    return lbl;
}

static void create_divider(lv_obj_t *parent)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
    lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
    lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_net_audio_create(void)
{
    scr_net_audio = lv_obj_create(NULL);
    lv_obj_add_style(scr_net_audio, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_net_audio, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_net_audio);
    lv_obj_remove_style_all(main);
    lv_obj_set_size(main, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_pad_gap(main, 0, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * HEADER BAR (48px)
     * ================================================================== */
    lv_obj_t *header = lv_obj_create(main);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, ui_theme_style_status_bar(), 0);
    lv_obj_set_size(header, UI_HOR_RES, 48);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 10, 0);

    /* Back button */
    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_add_style(btn_back, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_back, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn_back, 8, 0);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);

    /* Title */
    lv_obj_t *lbl_title = lv_label_create(header);
    lv_obj_add_style(lbl_title, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_title, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_title, "Net Audio");

    /* ==================================================================
     * SCROLLABLE CONTENT
     * ================================================================== */
    lv_obj_t *content = lv_obj_create(main);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, UI_HOR_RES);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, 0, 0);
    lv_obj_set_style_pad_bottom(content, 16, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

    /* Themed scrollbar */
    lv_obj_set_style_bg_color(content, lv_color_hex(0x333333), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, (lv_opa_t)120, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(content, 2, LV_PART_SCROLLBAR);

    /* ==================================================================
     * NOW STREAMING section (hidden when idle)
     * ================================================================== */
    lbl_stream_section = create_section(content, "NOW STREAMING", 16);

    card_stream = lv_obj_create(content);
    lv_obj_remove_style_all(card_stream);
    lv_obj_add_style(card_stream, ui_theme_style_card(), 0);
    lv_obj_set_size(card_stream, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(card_stream, 8, 0);
    lv_obj_set_flex_flow(card_stream, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_stream, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card_stream, 6, 0);
    lv_obj_set_scrollbar_mode(card_stream, LV_SCROLLBAR_MODE_OFF);

    /* Stream title */
    lbl_stream_title = lv_label_create(card_stream);
    lv_obj_add_style(lbl_stream_title, ui_theme_style_subtitle(), 0);
    lv_label_set_text(lbl_stream_title, "");
    lv_label_set_long_mode(lbl_stream_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_stream_title, CONTENT_W - 40);

    /* Format line (codec + sample rate + bit depth) */
    lbl_stream_format = lv_label_create(card_stream);
    lv_obj_add_style(lbl_stream_format, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_stream_format,
                                ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_stream_format, "");

    /* State line (colored dot + state text + elapsed) */
    lv_obj_t *state_row = lv_obj_create(card_stream);
    lv_obj_remove_style_all(state_row);
    lv_obj_set_size(state_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(state_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(state_row, 8, 0);
    lv_obj_set_scrollbar_mode(state_row, LV_SCROLLBAR_MODE_OFF);

    dot_stream_state = lv_obj_create(state_row);
    lv_obj_remove_style_all(dot_stream_state);
    lv_obj_set_size(dot_stream_state, 10, 10);
    lv_obj_set_style_radius(dot_stream_state, 5, 0);
    lv_obj_set_style_bg_color(dot_stream_state, COL_STATE_PAUSED, 0);
    lv_obj_set_style_bg_opa(dot_stream_state, LV_OPA_COVER, 0);

    lbl_stream_state = lv_label_create(state_row);
    lv_obj_add_style(lbl_stream_state, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_stream_state, "");

    /* Control buttons row */
    lv_obj_t *ctrl_row = lv_obj_create(card_stream);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_size(ctrl_row, CONTENT_W - 40, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ctrl_row, 12, 0);
    lv_obj_set_style_margin_top(ctrl_row, 8, 0);
    lv_obj_set_scrollbar_mode(ctrl_row, LV_SCROLLBAR_MODE_OFF);

    /* Pause/Resume button — outlined pill */
    btn_pause = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(btn_pause);
    lv_obj_set_style_bg_opa(btn_pause, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_pause, 2, 0);
    lv_obj_set_style_border_color(btn_pause, ui_theme_color_accent(), 0);
    lv_obj_set_style_radius(btn_pause, 24, 0);
    lv_obj_set_style_pad_hor(btn_pause, 20, 0);
    lv_obj_set_style_pad_ver(btn_pause, 10, 0);
    lv_obj_set_style_shadow_width(btn_pause, 0, 0);
    /* Pressed state */
    lv_obj_set_style_bg_color(btn_pause, ui_theme_color_accent(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_pause, LV_OPA_COVER, LV_STATE_PRESSED);

    lbl_pause_icon = lv_label_create(btn_pause);
    lv_obj_set_style_text_color(lbl_pause_icon, ui_theme_color_accent(), 0);
    lv_obj_set_style_text_font(lbl_pause_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_pause_icon, ui_theme_color_bg(),
                                LV_STATE_PRESSED);
    lv_label_set_text(lbl_pause_icon, LV_SYMBOL_PAUSE " Pause");
    lv_obj_add_event_cb(btn_pause, on_pause_resume, LV_EVENT_CLICKED, NULL);

    /* Stop button — outlined pill (gray) */
    btn_stop = lv_button_create(ctrl_row);
    lv_obj_remove_style_all(btn_stop);
    lv_obj_set_style_bg_opa(btn_stop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_stop, 2, 0);
    lv_obj_set_style_border_color(btn_stop, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(btn_stop, 24, 0);
    lv_obj_set_style_pad_hor(btn_stop, 20, 0);
    lv_obj_set_style_pad_ver(btn_stop, 10, 0);
    lv_obj_set_style_shadow_width(btn_stop, 0, 0);
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0x555555), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_stop, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *lbl_stop_icon = lv_label_create(btn_stop);
    lv_obj_set_style_text_color(lbl_stop_icon, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(lbl_stop_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_stop_icon, ui_theme_color_bg(),
                                LV_STATE_PRESSED);
    lv_label_set_text(lbl_stop_icon, LV_SYMBOL_STOP " Stop");
    lv_obj_add_event_cb(btn_stop, on_stop, LV_EVENT_CLICKED, NULL);

    /* Divider after streaming section */
    div_stream = lv_obj_create(content);
    lv_obj_remove_style_all(div_stream);
    lv_obj_add_style(div_stream, ui_theme_style_list_divider(), 0);
    lv_obj_set_size(div_stream, UI_HOR_RES - 2 * PAD_SIDE, 1);
    lv_obj_set_style_margin_left(div_stream, PAD_SIDE, 0);
    lv_obj_set_style_margin_top(div_stream, 12, 0);

    /* Start hidden */
    lv_obj_add_flag(lbl_stream_section, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_stream, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(div_stream, LV_OBJ_FLAG_HIDDEN);

    /* ==================================================================
     * PLAY URL section
     * ================================================================== */
    create_section(content, "PLAY URL", 16);

    /* URL textarea */
    ta_url = lv_textarea_create(content);
    lv_obj_set_size(ta_url, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(ta_url, 8, 0);
    lv_textarea_set_one_line(ta_url, true);
    lv_textarea_set_placeholder_text(ta_url, "https://stream.example.com/audio");
    lv_textarea_set_text(ta_url, "");

    /* Textarea styling */
    lv_obj_set_style_bg_color(ta_url, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(ta_url, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta_url, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(ta_url, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(ta_url, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(ta_url, 1, 0);
    lv_obj_set_style_radius(ta_url, 8, 0);
    lv_obj_set_style_pad_all(ta_url, 12, 0);
    /* Focused state */
    lv_obj_set_style_border_color(ta_url, ui_theme_color_accent(),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta_url, 2, LV_STATE_FOCUSED);
    /* Cursor color */
    lv_obj_set_style_bg_color(ta_url, ui_theme_color_accent(), LV_PART_CURSOR);

    lv_obj_add_event_cb(ta_url, on_ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta_url, on_ta_defocus, LV_EVENT_DEFOCUSED, NULL);

    /* Play URL button — gold filled pill */
    btn_play_url = lv_button_create(content);
    lv_obj_remove_style_all(btn_play_url);
    lv_obj_set_style_bg_color(btn_play_url, ui_theme_color_accent(), 0);
    lv_obj_set_style_bg_opa(btn_play_url, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_play_url, 24, 0);
    lv_obj_set_style_pad_hor(btn_play_url, 24, 0);
    lv_obj_set_style_pad_ver(btn_play_url, 12, 0);
    lv_obj_set_style_shadow_width(btn_play_url, 0, 0);
    lv_obj_set_style_margin_top(btn_play_url, 12, 0);
    /* Pressed state */
    lv_obj_set_style_bg_color(btn_play_url, lv_color_hex(0xC48E1F),
                              LV_STATE_PRESSED);

    lv_obj_t *lbl_play_url = lv_label_create(btn_play_url);
    lv_obj_set_style_text_color(lbl_play_url, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_text_font(lbl_play_url, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_play_url, LV_SYMBOL_PLAY " Play URL");
    lv_obj_add_event_cb(btn_play_url, on_play_url, LV_EVENT_CLICKED, NULL);

    /* ==================================================================
     * RADIO STATIONS section
     * ================================================================== */
    create_section(content, "RADIO STATIONS", 24);
    create_divider(content);

    /* Container for dynamically built preset rows */
    preset_list = lv_obj_create(content);
    lv_obj_remove_style_all(preset_list);
    lv_obj_set_width(preset_list, UI_HOR_RES);
    lv_obj_set_height(preset_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(preset_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preset_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(preset_list, 0, 0);
    lv_obj_set_scrollbar_mode(preset_list, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * KEYBOARD (hidden by default, shown on textarea focus)
     * ================================================================== */
    kb_url = lv_keyboard_create(main);
    lv_keyboard_set_textarea(kb_url, ta_url);
    lv_keyboard_set_mode(kb_url, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(kb_url, UI_HOR_RES, UI_VER_RES / 3);
    lv_obj_add_flag(kb_url, LV_OBJ_FLAG_HIDDEN);

    /* Keyboard styling (match WiFi keyboard) */
    lv_obj_set_style_bg_color(kb_url, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(kb_url, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb_url, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(kb_url, 1, 0);
    lv_obj_set_style_border_side(kb_url, LV_BORDER_SIDE_TOP, 0);

    /* Key-level styling */
    lv_obj_set_style_bg_color(kb_url, lv_color_hex(0x252525),
                              (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb_url, LV_OPA_COVER,
                            (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb_url, lv_color_hex(0x3A3A3A),
                                  (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb_url, 1,
                                  (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_radius(kb_url, 8,
                            (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb_url, lv_color_hex(0xFFFFFF),
                                (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb_url, &lv_font_montserrat_16,
                               (uint32_t)LV_PART_ITEMS);
    /* Pressed keys */
    lv_obj_set_style_bg_color(kb_url, lv_color_hex(0x3A3A3A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_PRESSED);
    /* Checked keys (special keys like ABC, 123) */
    lv_obj_set_style_bg_color(kb_url, lv_color_hex(0x1A1A1A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb_url, ui_theme_color_accent(),
                                (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_add_event_cb(kb_url, on_kb_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(kb_url, on_kb_event, LV_EVENT_READY, NULL);

    /* Init tracking state */
    s_prev_na_state = UI_NET_AUDIO_IDLE;
    s_toast_seeded = false;
    s_last_preset_count = -1;

    return scr_net_audio;
}

/* -----------------------------------------------------------------------
 * Build preset list rows
 * ----------------------------------------------------------------------- */

static void build_preset_list(const ui_net_audio_data_t *data)
{
    lv_obj_clean(preset_list);

    for (int i = 0; i < data->preset_count; i++) {
        const ui_radio_preset_t *preset = &data->presets[i];

        /* Row container (clickable) */
        lv_obj_t *row = lv_obj_create(preset_list);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, ui_theme_style_list_item(), 0);
        lv_obj_set_size(row, UI_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 12, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

        /* Pressed feedback */
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        /* Radio icon */
        lv_obj_t *icon = lv_label_create(row);
        lv_obj_add_style(icon, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(icon, ui_theme_color_accent(), 0);
        lv_label_set_text(icon, LV_SYMBOL_AUDIO);

        /* Station name (flex grow) */
        lv_obj_t *name = lv_label_create(row);
        lv_obj_add_style(name, ui_theme_style_list_item(), 0);
        lv_label_set_text(name, preset->name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);

        /* Codec chip */
        lv_obj_t *codec_lbl = lv_label_create(row);
        lv_obj_add_style(codec_lbl, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(codec_lbl,
                                    ui_theme_color_text_secondary(), 0);
        /* Uppercase the codec hint for display */
        char codec_upper[16];
        strncpy(codec_upper, preset->codec_hint, sizeof(codec_upper) - 1);
        codec_upper[sizeof(codec_upper) - 1] = '\0';
        for (int j = 0; codec_upper[j]; j++) {
            if (codec_upper[j] >= 'a' && codec_upper[j] <= 'z')
                codec_upper[j] -= 32;
        }
        lv_label_set_text(codec_lbl, codec_upper);

        /* Click event — pass URL as user_data (preset is const static) */
        lv_obj_add_event_cb(row, on_preset_click, LV_EVENT_CLICKED,
                            (void *)preset->url);

        /* Divider (skip after last) */
        if (i < data->preset_count - 1) {
            lv_obj_t *div = lv_obj_create(preset_list);
            lv_obj_remove_style_all(div);
            lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
            lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
            lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
        }
    }

    s_last_preset_count = data->preset_count;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh data)
 * ----------------------------------------------------------------------- */

void ui_net_audio_update(const ui_net_audio_data_t *data)
{
    if (!scr_net_audio) return;

    /* -- Build preset list on first call or if count changes -- */
    if (data->preset_count != s_last_preset_count) {
        build_preset_list(data);
    }

    /* -- Show/hide streaming card based on state -- */
    bool active = (data->state != UI_NET_AUDIO_IDLE);
    if (active) {
        lv_obj_clear_flag(lbl_stream_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(card_stream, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(div_stream, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_stream_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_stream, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(div_stream, LV_OBJ_FLAG_HIDDEN);
    }

    if (active) {
        /* Stream title */
        lv_label_set_text(lbl_stream_title,
                          data->stream_title[0] ? data->stream_title
                                                : "Unknown Stream");

        /* Format line */
        char fmt_buf[64];
        if (data->sample_rate > 0) {
            uint32_t sr_k = data->sample_rate / 1000;
            uint32_t sr_frac = (data->sample_rate % 1000) / 100;
            if (data->bitrate_kbps > 0) {
                if (sr_frac)
                    lv_snprintf(fmt_buf, sizeof(fmt_buf),
                                "%s \xc2\xb7 %lu.%lu kHz \xc2\xb7 %d-bit \xc2\xb7 %lu kbps",
                                data->codec, (unsigned long)sr_k,
                                (unsigned long)sr_frac, data->bits_per_sample,
                                (unsigned long)data->bitrate_kbps);
                else
                    lv_snprintf(fmt_buf, sizeof(fmt_buf),
                                "%s \xc2\xb7 %lu kHz \xc2\xb7 %d-bit \xc2\xb7 %lu kbps",
                                data->codec, (unsigned long)sr_k,
                                data->bits_per_sample,
                                (unsigned long)data->bitrate_kbps);
            } else {
                if (sr_frac)
                    lv_snprintf(fmt_buf, sizeof(fmt_buf),
                                "%s \xc2\xb7 %lu.%lu kHz \xc2\xb7 %d-bit",
                                data->codec, (unsigned long)sr_k,
                                (unsigned long)sr_frac, data->bits_per_sample);
                else
                    lv_snprintf(fmt_buf, sizeof(fmt_buf),
                                "%s \xc2\xb7 %lu kHz \xc2\xb7 %d-bit",
                                data->codec, (unsigned long)sr_k,
                                data->bits_per_sample);
            }
        } else {
            lv_snprintf(fmt_buf, sizeof(fmt_buf), "%s", data->codec);
        }
        lv_label_set_text(lbl_stream_format, fmt_buf);

        /* State line with colored dot */
        char state_buf[96];
        lv_color_t state_col;

        switch (data->state) {
            case UI_NET_AUDIO_CONNECTING:
                lv_snprintf(state_buf, sizeof(state_buf), "Connecting...");
                state_col = COL_STATE_BUFFERING;
                break;
            case UI_NET_AUDIO_BUFFERING:
                lv_snprintf(state_buf, sizeof(state_buf), "Buffering...");
                state_col = COL_STATE_BUFFERING;
                break;
            case UI_NET_AUDIO_PLAYING: {
                char t_buf[16];
                ui_fmt_time(t_buf, sizeof(t_buf), data->elapsed_ms);
                lv_snprintf(state_buf, sizeof(state_buf),
                            "Playing \xc2\xb7 %s", t_buf);
                state_col = COL_STATE_PLAYING;
                break;
            }
            case UI_NET_AUDIO_PAUSED: {
                char t_buf[16];
                ui_fmt_time(t_buf, sizeof(t_buf), data->elapsed_ms);
                lv_snprintf(state_buf, sizeof(state_buf),
                            "Paused \xc2\xb7 %s", t_buf);
                state_col = COL_STATE_PAUSED;
                break;
            }
            case UI_NET_AUDIO_ERROR:
                lv_snprintf(state_buf, sizeof(state_buf), "%s",
                            data->error_msg[0] ? data->error_msg : "Error");
                state_col = COL_STATE_ERROR;
                break;
            default:
                state_buf[0] = '\0';
                state_col = COL_STATE_PAUSED;
                break;
        }
        lv_label_set_text(lbl_stream_state, state_buf);
        lv_obj_set_style_text_color(lbl_stream_state, state_col, 0);
        lv_obj_set_style_bg_color(dot_stream_state, state_col, 0);

        /* Pause/Resume button label */
        bool can_pause = (data->state == UI_NET_AUDIO_PLAYING);
        bool can_resume = (data->state == UI_NET_AUDIO_PAUSED);
        if (can_pause) {
            lv_label_set_text(lbl_pause_icon, LV_SYMBOL_PAUSE " Pause");
        } else if (can_resume) {
            lv_label_set_text(lbl_pause_icon, LV_SYMBOL_PLAY " Resume");
        } else {
            lv_label_set_text(lbl_pause_icon, LV_SYMBOL_PAUSE " Pause");
        }
    }

    /* -- Toast wiring -- */
    if (!s_toast_seeded) {
        s_toast_seeded = true;
        s_prev_na_state = data->state;
    } else if (data->state != s_prev_na_state) {
        if (data->state == UI_NET_AUDIO_PLAYING) {
            char msg[160];
            lv_snprintf(msg, sizeof(msg), "Streaming: %s",
                        data->stream_title[0] ? data->stream_title : "Stream");
            ui_toast_show(UI_TOAST_SUCCESS, msg, 3000);
        } else if (data->state == UI_NET_AUDIO_ERROR) {
            ui_toast_show(UI_TOAST_ERROR,
                          data->error_msg[0] ? data->error_msg
                                             : "Stream error", 4000);
        } else if (data->state == UI_NET_AUDIO_IDLE &&
                   s_prev_na_state != UI_NET_AUDIO_IDLE) {
            ui_toast_show(UI_TOAST_INFO, "Stream stopped", 2500);
        }
        s_prev_na_state = data->state;
    }
}
