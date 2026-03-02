/*
 * ui_eq.c — Dedicated EQ sub-screen for Lyra (720x1280 portrait).
 *
 * Layout (top to bottom):
 *
 *   ┌──────────────────────────────────┐
 *   │  ◀  Equalizer                    │  Header (48px)
 *   ├──────────────────────────────────┤
 *   │                                  │
 *   │   60    250    1k    4k   16k    │  Freq labels
 *   │    │      │     │     │     │    │
 *   │    ┃      ┃     ┃     ┃     ┃    │  5 vertical sliders
 *   │    ┃      ┃     ┃     ┃     ┃    │  -12 to +12 dB
 *   │    │      │     │     │     │    │
 *   │  +3dB   0dB  -2dB  +4dB  +6dB   │  dB readout per band
 *   │                                  │
 *   │  PRESETS                         │  Section
 *   │  [Flat] [Rock] [Jazz] [Bass]    │  Built-in chips
 *   │  [User1] [User2] [+]            │  User chips + add button
 *   │                                  │
 *   │  DSP PROCESSING                  │  Section
 *   │  Enable DSP          [====]      │  Toggle
 *   │                                  │
 *   │  [Reset to Flat]                 │  Reset button
 *   └──────────────────────────────────┘
 *
 * No bottom nav bar (sub-screen — back returns to Now Playing).
 */

#include "ui_internal.h"
#include <string.h>

/* Layout constants */
#define PAD_SIDE       24
#define CONTENT_W      (UI_HOR_RES - 2 * PAD_SIDE)
#define EQ_SLIDER_H   200     /* Height of vertical EQ sliders */

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_eq;

/* EQ sliders + labels */
static lv_obj_t *eq_sliders[UI_EQ_BANDS];
static lv_obj_t *eq_val_labels[UI_EQ_BANDS];

/* Built-in presets */
static lv_obj_t *preset_btns[4];
static const char *preset_names[] = { "Flat", "Rock", "Jazz", "Bass" };
#define NUM_BUILTIN_PRESETS  4

/* User presets */
static lv_obj_t *user_preset_row;
static lv_obj_t *user_chip_objs[UI_MAX_USER_PRESETS];
static char s_user_names[UI_MAX_USER_PRESETS][UI_PRESET_NAME_LEN];
static int  s_user_chip_count = -1;  /* Force initial build */

/* DSP toggle */
static lv_obj_t *sw_dsp;
static lv_obj_t *lbl_dsp_state;

/* Band index stored as user_data for slider events */
static int s_band_idx[UI_EQ_BANDS] = { 0, 1, 2, 3, 4 };

/* Prevents slider events from firing during update() */
static bool s_updating = false;

/* Name input dialog (for saving user presets) */
static lv_obj_t *dlg_name;
static lv_obj_t *ta_name;
static lv_obj_t *kb_name;

/* Delete confirmation dialog */
static lv_obj_t *dlg_delete;
static lv_obj_t *lbl_delete_msg;
static char s_delete_name[UI_PRESET_NAME_LEN];

/* -----------------------------------------------------------------------
 * Helpers
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

/* -----------------------------------------------------------------------
 * Dialog helpers
 * ----------------------------------------------------------------------- */

static void show_name_dialog(void)
{
    lv_textarea_set_text(ta_name, "");
    lv_obj_clear_flag(dlg_name, LV_OBJ_FLAG_HIDDEN);
}

static void hide_name_dialog(void)
{
    lv_obj_add_flag(dlg_name, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ta_name, "");
}

static void show_delete_dialog(const char *name)
{
    strncpy(s_delete_name, name, UI_PRESET_NAME_LEN - 1);
    s_delete_name[UI_PRESET_NAME_LEN - 1] = '\0';
    char buf[64];
    lv_snprintf(buf, sizeof(buf), "Delete \"%s\"?", name);
    lv_label_set_text(lbl_delete_msg, buf);
    lv_obj_clear_flag(dlg_delete, LV_OBJ_FLAG_HIDDEN);
}

static void hide_delete_dialog(void)
{
    lv_obj_add_flag(dlg_delete, LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    hide_name_dialog();
    hide_delete_dialog();
    ui_navigate_to(UI_SCREEN_NOW_PLAYING);
}

static void on_eq_slider(lv_event_t *e)
{
    if (s_updating) return;
    int *band = (int *)lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    ui_cmd_set_eq_band(*band, (int8_t)val);
}

static void on_builtin_preset_click(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    ui_cmd_set_dsp_preset(name);
}

static void on_user_preset_click(lv_event_t *e)
{
    char *name = (char *)lv_event_get_user_data(e);
    ui_cmd_set_dsp_preset(name);
}

static void on_user_preset_long_press(lv_event_t *e)
{
    char *name = (char *)lv_event_get_user_data(e);
    show_delete_dialog(name);
}

static void on_add_preset(lv_event_t *e)
{
    (void)e;
    show_name_dialog();
}

static void on_dsp_toggle(lv_event_t *e)
{
    (void)e;
    ui_cmd_toggle_dsp();
}

static void on_reset_flat(lv_event_t *e)
{
    (void)e;
    ui_cmd_set_dsp_preset("Flat");
    ui_toast_show(UI_TOAST_INFO, "Reset to Flat", 2000);
}

/* Name dialog callbacks */
static void on_name_save(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(ta_name);
    if (name && name[0]) {
        ui_cmd_save_eq_preset(name);
        ui_toast_show(UI_TOAST_SUCCESS, "Preset saved", 2000);
        s_user_chip_count = -1;  /* Force rebuild */
    }
    hide_name_dialog();
}

static void on_name_cancel(lv_event_t *e)
{
    (void)e;
    hide_name_dialog();
}

static void on_kb_name_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        on_name_save(e);
    } else if (code == LV_EVENT_CANCEL) {
        hide_name_dialog();
    }
}

/* Delete dialog callbacks */
static void on_delete_confirm(lv_event_t *e)
{
    (void)e;
    ui_cmd_delete_eq_preset(s_delete_name);
    ui_toast_show(UI_TOAST_INFO, "Preset deleted", 2000);
    s_user_chip_count = -1;  /* Force rebuild */
    hide_delete_dialog();
}

static void on_delete_cancel(lv_event_t *e)
{
    (void)e;
    hide_delete_dialog();
}

/* -----------------------------------------------------------------------
 * Rebuild user preset chips
 * ----------------------------------------------------------------------- */

static void rebuild_user_chips(const ui_eq_presets_data_t *presets,
                               const char *active_preset, bool dsp_on)
{
    lv_obj_clean(user_preset_row);

    for (int i = 0; i < presets->count && i < UI_MAX_USER_PRESETS; i++) {
        /* Copy name for stable user_data */
        strncpy(s_user_names[i], presets->presets[i].name,
                UI_PRESET_NAME_LEN - 1);
        s_user_names[i][UI_PRESET_NAME_LEN - 1] = '\0';

        user_chip_objs[i] = lv_button_create(user_preset_row);
        lv_obj_add_style(user_chip_objs[i], ui_theme_style_preset_chip(), 0);
        lv_obj_add_style(user_chip_objs[i],
                         ui_theme_style_preset_chip_active(), LV_STATE_CHECKED);
        lv_obj_add_flag(user_chip_objs[i], LV_OBJ_FLAG_CHECKABLE);

        /* Check if this is the active preset */
        bool active = dsp_on && active_preset &&
                      strcmp(active_preset, s_user_names[i]) == 0;
        if (active)
            lv_obj_add_state(user_chip_objs[i], LV_STATE_CHECKED);

        lv_obj_t *lbl = lv_label_create(user_chip_objs[i]);
        lv_label_set_text(lbl, s_user_names[i]);

        lv_obj_add_event_cb(user_chip_objs[i], on_user_preset_click,
                            LV_EVENT_CLICKED, s_user_names[i]);
        lv_obj_add_event_cb(user_chip_objs[i], on_user_preset_long_press,
                            LV_EVENT_LONG_PRESSED, s_user_names[i]);
    }

    /* "+" add button */
    if (presets->count < UI_MAX_USER_PRESETS) {
        lv_obj_t *btn_add = lv_button_create(user_preset_row);
        lv_obj_add_style(btn_add, ui_theme_style_preset_chip(), 0);
        lv_obj_set_style_border_color(btn_add, lv_color_hex(0x3A3A3A), 0);
        lv_obj_set_style_border_width(btn_add, 1, 0);
        lv_obj_set_style_border_opa(btn_add, LV_OPA_COVER, 0);
        /* Pressed feedback */
        lv_obj_set_style_bg_color(btn_add, lv_color_hex(0x333333),
                                  LV_STATE_PRESSED);

        lv_obj_t *lbl = lv_label_create(btn_add);
        lv_label_set_text(lbl, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_color(lbl, ui_theme_color_accent(), 0);

        lv_obj_add_event_cb(btn_add, on_add_preset, LV_EVENT_CLICKED, NULL);
    }

    s_user_chip_count = presets->count;
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_eq_create(void)
{
    scr_eq = lv_obj_create(NULL);
    lv_obj_add_style(scr_eq, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_eq, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main_cont = lv_obj_create(scr_eq);
    lv_obj_remove_style_all(main_cont);
    lv_obj_set_size(main_cont, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main_cont, 0, 0);
    lv_obj_set_style_pad_gap(main_cont, 0, 0);
    lv_obj_set_scrollbar_mode(main_cont, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * HEADER BAR (48px)
     * ================================================================== */
    lv_obj_t *header = lv_obj_create(main_cont);
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
    lv_label_set_text(lbl_title, "Equalizer");

    /* ==================================================================
     * SCROLLABLE CONTENT
     * ================================================================== */
    lv_obj_t *content = lv_obj_create(main_cont);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, UI_HOR_RES);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, 0, 0);
    lv_obj_set_style_pad_bottom(content, 24, 0);
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
     * EQUALIZER SLIDERS
     * ================================================================== */
    static const char *eq_freq_labels[UI_EQ_BANDS] = {
        "60Hz", "250Hz", "1kHz", "4kHz", "16kHz"
    };

    lv_obj_t *eq_area = lv_obj_create(content);
    lv_obj_remove_style_all(eq_area);
    lv_obj_set_size(eq_area, CONTENT_W, EQ_SLIDER_H + 70);
    lv_obj_set_style_margin_top(eq_area, 16, 0);
    lv_obj_set_flex_flow(eq_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_area, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(eq_area, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < UI_EQ_BANDS; i++) {
        /* Column per band */
        lv_obj_t *col = lv_obj_create(eq_area);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 90, EQ_SLIDER_H + 70);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_gap(col, 6, 0);

        /* Frequency label on top */
        lv_obj_t *freq_lbl = lv_label_create(col);
        lv_obj_add_style(freq_lbl, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(freq_lbl,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(freq_lbl, eq_freq_labels[i]);

        /* Vertical slider */
        eq_sliders[i] = lv_slider_create(col);
        lv_obj_set_size(eq_sliders[i], 30, EQ_SLIDER_H);
        lv_slider_set_range(eq_sliders[i], -12, 12);
        lv_slider_set_value(eq_sliders[i], 0, LV_ANIM_OFF);

        /* Track styling */
        lv_obj_set_style_bg_color(eq_sliders[i],
                                  ui_theme_color_slider_track(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(eq_sliders[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(eq_sliders[i], 4, LV_PART_MAIN);

        /* Indicator (filled portion) */
        lv_obj_add_style(eq_sliders[i], ui_theme_style_slider_main(),
                         LV_PART_INDICATOR);

        /* Knob */
        lv_obj_add_style(eq_sliders[i], ui_theme_style_slider_knob(),
                         LV_PART_KNOB);

        /* Event callback with band index */
        lv_obj_add_event_cb(eq_sliders[i], on_eq_slider,
                            LV_EVENT_VALUE_CHANGED, &s_band_idx[i]);

        /* dB value label below slider */
        eq_val_labels[i] = lv_label_create(col);
        lv_obj_add_style(eq_val_labels[i], ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(eq_val_labels[i],
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(eq_val_labels[i], "0 dB");
        lv_obj_set_style_text_align(eq_val_labels[i],
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(eq_val_labels[i], 60);
    }

    /* ==================================================================
     * PRESETS section
     * ================================================================== */
    create_section(content, "PRESETS", 20);

    /* Built-in preset row */
    lv_obj_t *builtin_row = lv_obj_create(content);
    lv_obj_remove_style_all(builtin_row);
    lv_obj_set_size(builtin_row, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(builtin_row, 8, 0);
    lv_obj_set_flex_flow(builtin_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(builtin_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(builtin_row, 10, 0);
    lv_obj_set_scrollbar_mode(builtin_row, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < NUM_BUILTIN_PRESETS; i++) {
        preset_btns[i] = lv_button_create(builtin_row);
        lv_obj_add_style(preset_btns[i], ui_theme_style_preset_chip(), 0);
        lv_obj_add_style(preset_btns[i], ui_theme_style_preset_chip_active(),
                         LV_STATE_CHECKED);
        lv_obj_add_flag(preset_btns[i], LV_OBJ_FLAG_CHECKABLE);

        lv_obj_t *lbl = lv_label_create(preset_btns[i]);
        lv_label_set_text(lbl, preset_names[i]);

        lv_obj_add_event_cb(preset_btns[i], on_builtin_preset_click,
                            LV_EVENT_CLICKED, (void *)preset_names[i]);
    }

    /* User preset row (dynamically populated) */
    lv_obj_t *user_section_lbl = lv_label_create(content);
    lv_obj_add_style(user_section_lbl, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(user_section_lbl,
                                ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_margin_top(user_section_lbl, 16, 0);
    lv_obj_set_style_pad_left(user_section_lbl, PAD_SIDE, 0);
    lv_obj_set_width(user_section_lbl, UI_HOR_RES);
    lv_label_set_text(user_section_lbl, "My Presets");

    user_preset_row = lv_obj_create(content);
    lv_obj_remove_style_all(user_preset_row);
    lv_obj_set_size(user_preset_row, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(user_preset_row, 40, 0);
    lv_obj_set_style_margin_top(user_preset_row, 8, 0);
    lv_obj_set_flex_flow(user_preset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(user_preset_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(user_preset_row, 10, 0);
    lv_obj_set_style_pad_left(user_preset_row, PAD_SIDE, 0);
    lv_obj_set_scrollbar_mode(user_preset_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(user_preset_row, LV_OBJ_FLAG_SCROLL_ONE);

    /* ==================================================================
     * DSP PROCESSING section
     * ================================================================== */
    create_section(content, "DSP PROCESSING", 24);

    lv_obj_t *dsp_card = lv_obj_create(content);
    lv_obj_remove_style_all(dsp_card);
    lv_obj_add_style(dsp_card, ui_theme_style_card(), 0);
    lv_obj_set_size(dsp_card, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(dsp_card, 8, 0);
    lv_obj_set_flex_flow(dsp_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dsp_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(dsp_card, 12, 0);
    lv_obj_set_scrollbar_mode(dsp_card, LV_SCROLLBAR_MODE_OFF);

    /* Toggle row */
    lv_obj_t *toggle_row = lv_obj_create(dsp_card);
    lv_obj_remove_style_all(toggle_row);
    lv_obj_set_size(toggle_row, CONTENT_W - 40, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(toggle_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *dsp_left = lv_obj_create(toggle_row);
    lv_obj_remove_style_all(dsp_left);
    lv_obj_set_size(dsp_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dsp_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(dsp_left, 8, 0);
    lv_obj_set_scrollbar_mode(dsp_left, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl_dsp = lv_label_create(dsp_left);
    lv_obj_add_style(lbl_dsp, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_dsp, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(lbl_dsp, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_dsp, "Enable DSP");

    lbl_dsp_state = lv_label_create(dsp_left);
    lv_obj_add_style(lbl_dsp_state, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_dsp_state, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_dsp_state, "ON");

    sw_dsp = lv_switch_create(toggle_row);
    lv_obj_set_size(sw_dsp, 50, 26);
    lv_obj_set_style_bg_color(sw_dsp, ui_theme_color_slider_track(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw_dsp, ui_theme_color_accent(),
                              (uint32_t)LV_PART_INDICATOR | (uint32_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw_dsp, ui_theme_color_text_primary(),
                              LV_PART_KNOB);
    lv_obj_add_event_cb(sw_dsp, on_dsp_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    /* ==================================================================
     * RESET BUTTON
     * ================================================================== */
    lv_obj_t *reset_row = lv_obj_create(content);
    lv_obj_remove_style_all(reset_row);
    lv_obj_set_size(reset_row, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(reset_row, 24, 0);
    lv_obj_set_flex_flow(reset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(reset_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *btn_reset = lv_button_create(reset_row);
    lv_obj_set_style_bg_opa(btn_reset, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn_reset, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_reset, 1, 0);
    lv_obj_set_style_border_opa(btn_reset, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_reset, 24, 0);
    lv_obj_set_style_pad_hor(btn_reset, 28, 0);
    lv_obj_set_style_pad_ver(btn_reset, 10, 0);
    lv_obj_set_style_shadow_width(btn_reset, 0, 0);
    /* Pressed feedback */
    lv_obj_set_style_bg_color(btn_reset, lv_color_hex(0x333333),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_reset, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_obj_set_style_text_color(lbl_reset, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_font(lbl_reset, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_reset, "Reset to Flat");
    lv_obj_add_event_cb(btn_reset, on_reset_flat, LV_EVENT_CLICKED, NULL);

    /* ==================================================================
     * NAME INPUT DIALOG (overlay, hidden by default)
     * ================================================================== */
    dlg_name = lv_obj_create(scr_eq);
    lv_obj_remove_style_all(dlg_name);
    lv_obj_set_size(dlg_name, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_style_bg_color(dlg_name, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dlg_name, LV_OPA_80, 0);
    lv_obj_set_flex_flow(dlg_name, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg_name, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(dlg_name, 0, 0);
    lv_obj_add_flag(dlg_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(dlg_name, LV_SCROLLBAR_MODE_OFF);

    /* Dialog card */
    lv_obj_t *name_card = lv_obj_create(dlg_name);
    lv_obj_remove_style_all(name_card);
    lv_obj_add_style(name_card, ui_theme_style_card(), 0);
    lv_obj_set_size(name_card, UI_HOR_RES - 48, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(name_card, 24, 0);
    lv_obj_set_flex_flow(name_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(name_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(name_card, 16, 0);
    lv_obj_set_scrollbar_mode(name_card, LV_SCROLLBAR_MODE_OFF);

    /* Dialog title */
    lv_obj_t *lbl_dlg_title = lv_label_create(name_card);
    lv_obj_add_style(lbl_dlg_title, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_dlg_title,
                                ui_theme_color_text_primary(), 0);
    lv_label_set_text(lbl_dlg_title, "Save Preset");
    lv_obj_set_width(lbl_dlg_title, lv_pct(100));
    lv_obj_set_style_text_align(lbl_dlg_title, LV_TEXT_ALIGN_CENTER, 0);

    /* Name textarea */
    ta_name = lv_textarea_create(name_card);
    lv_obj_set_size(ta_name, lv_pct(100), LV_SIZE_CONTENT);
    lv_textarea_set_placeholder_text(ta_name, "Preset name");
    lv_textarea_set_one_line(ta_name, true);
    lv_textarea_set_max_length(ta_name, UI_PRESET_NAME_LEN - 1);
    lv_obj_set_style_bg_color(ta_name, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(ta_name, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta_name, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_border_color(ta_name, ui_theme_color_accent(), 0);
    lv_obj_set_style_border_width(ta_name, 1, 0);
    lv_obj_set_style_radius(ta_name, 6, 0);
    lv_obj_set_style_pad_all(ta_name, 12, 0);

    /* Button row */
    lv_obj_t *name_btn_row = lv_obj_create(name_card);
    lv_obj_remove_style_all(name_btn_row);
    lv_obj_set_size(name_btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(name_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(name_btn_row, LV_SCROLLBAR_MODE_OFF);

    /* Cancel button */
    lv_obj_t *btn_name_cancel = lv_button_create(name_btn_row);
    lv_obj_set_style_bg_opa(btn_name_cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn_name_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_name_cancel, 1, 0);
    lv_obj_set_style_border_opa(btn_name_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_name_cancel, 24, 0);
    lv_obj_set_style_pad_hor(btn_name_cancel, 32, 0);
    lv_obj_set_style_pad_ver(btn_name_cancel, 10, 0);
    lv_obj_set_style_shadow_width(btn_name_cancel, 0, 0);
    lv_obj_set_style_bg_color(btn_name_cancel, lv_color_hex(0x333333),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_name_cancel, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *lbl_nc = lv_label_create(btn_name_cancel);
    lv_obj_set_style_text_color(lbl_nc, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_font(lbl_nc, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_nc, "Cancel");
    lv_obj_add_event_cb(btn_name_cancel, on_name_cancel,
                        LV_EVENT_CLICKED, NULL);

    /* Save button — filled gold pill */
    lv_obj_t *btn_name_save = lv_button_create(name_btn_row);
    lv_obj_set_style_bg_color(btn_name_save, ui_theme_color_accent(), 0);
    lv_obj_set_style_bg_opa(btn_name_save, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_name_save, 24, 0);
    lv_obj_set_style_pad_hor(btn_name_save, 32, 0);
    lv_obj_set_style_pad_ver(btn_name_save, 10, 0);
    lv_obj_set_style_border_width(btn_name_save, 0, 0);
    lv_obj_set_style_shadow_width(btn_name_save, 0, 0);
    lv_obj_set_style_bg_color(btn_name_save, lv_color_hex(0xC48E1F),
                              LV_STATE_PRESSED);
    lv_obj_t *lbl_ns = lv_label_create(btn_name_save);
    lv_obj_set_style_text_color(lbl_ns, ui_theme_color_bg(), 0);
    lv_obj_set_style_text_font(lbl_ns, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_ns, "Save");
    lv_obj_add_event_cb(btn_name_save, on_name_save, LV_EVENT_CLICKED, NULL);

    /* Keyboard */
    kb_name = lv_keyboard_create(dlg_name);
    lv_obj_set_size(kb_name, UI_HOR_RES, 400);
    lv_keyboard_set_textarea(kb_name, ta_name);

    /* Dark keyboard styling */
    lv_obj_set_style_bg_color(kb_name, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(kb_name, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb_name, 0, 0);
    lv_obj_set_style_pad_all(kb_name, 6, 0);
    lv_obj_set_style_pad_gap(kb_name, 4, 0);

    lv_obj_set_style_bg_color(kb_name, lv_color_hex(0x252525), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb_name, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb_name, lv_color_hex(0x3A3A3A), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb_name, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(kb_name, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb_name, 8, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb_name, ui_theme_color_text_primary(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb_name, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb_name, 0, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(kb_name, lv_color_hex(0x3A3A3A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(kb_name, lv_color_hex(0x555555),
                                  (uint32_t)LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_obj_set_style_bg_color(kb_name, lv_color_hex(0x1A1A1A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb_name, ui_theme_color_accent(),
                                (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(kb_name, lv_color_hex(0x333333),
                                  (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_add_event_cb(kb_name, on_kb_name_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb_name, on_kb_name_event, LV_EVENT_CANCEL, NULL);

    /* ==================================================================
     * DELETE CONFIRMATION DIALOG (overlay, hidden by default)
     * ================================================================== */
    dlg_delete = lv_obj_create(scr_eq);
    lv_obj_remove_style_all(dlg_delete);
    lv_obj_set_size(dlg_delete, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_style_bg_color(dlg_delete, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dlg_delete, LV_OPA_80, 0);
    lv_obj_set_flex_flow(dlg_delete, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg_delete, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(dlg_delete, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scrollbar_mode(dlg_delete, LV_SCROLLBAR_MODE_OFF);

    /* Delete dialog card */
    lv_obj_t *del_card = lv_obj_create(dlg_delete);
    lv_obj_remove_style_all(del_card);
    lv_obj_add_style(del_card, ui_theme_style_card(), 0);
    lv_obj_set_size(del_card, UI_HOR_RES - 80, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(del_card, 24, 0);
    lv_obj_set_flex_flow(del_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(del_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(del_card, 20, 0);
    lv_obj_set_scrollbar_mode(del_card, LV_SCROLLBAR_MODE_OFF);

    /* Delete message */
    lbl_delete_msg = lv_label_create(del_card);
    lv_obj_add_style(lbl_delete_msg, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_delete_msg,
                                ui_theme_color_text_primary(), 0);
    lv_label_set_text(lbl_delete_msg, "Delete preset?");
    lv_obj_set_width(lbl_delete_msg, lv_pct(100));
    lv_obj_set_style_text_align(lbl_delete_msg, LV_TEXT_ALIGN_CENTER, 0);

    /* Delete button row */
    lv_obj_t *del_btn_row = lv_obj_create(del_card);
    lv_obj_remove_style_all(del_btn_row);
    lv_obj_set_size(del_btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(del_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(del_btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(del_btn_row, LV_SCROLLBAR_MODE_OFF);

    /* Cancel */
    lv_obj_t *btn_del_cancel = lv_button_create(del_btn_row);
    lv_obj_set_style_bg_opa(btn_del_cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn_del_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_del_cancel, 1, 0);
    lv_obj_set_style_border_opa(btn_del_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_del_cancel, 24, 0);
    lv_obj_set_style_pad_hor(btn_del_cancel, 28, 0);
    lv_obj_set_style_pad_ver(btn_del_cancel, 10, 0);
    lv_obj_set_style_shadow_width(btn_del_cancel, 0, 0);
    lv_obj_set_style_bg_color(btn_del_cancel, lv_color_hex(0x333333),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_del_cancel, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *lbl_dc = lv_label_create(btn_del_cancel);
    lv_obj_set_style_text_color(lbl_dc, ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_font(lbl_dc, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_dc, "Cancel");
    lv_obj_add_event_cb(btn_del_cancel, on_delete_cancel,
                        LV_EVENT_CLICKED, NULL);

    /* Delete button — red filled pill */
    lv_obj_t *btn_del_confirm = lv_button_create(del_btn_row);
    lv_obj_set_style_bg_color(btn_del_confirm, lv_color_hex(0xD32F2F), 0);
    lv_obj_set_style_bg_opa(btn_del_confirm, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_del_confirm, 24, 0);
    lv_obj_set_style_pad_hor(btn_del_confirm, 28, 0);
    lv_obj_set_style_pad_ver(btn_del_confirm, 10, 0);
    lv_obj_set_style_border_width(btn_del_confirm, 0, 0);
    lv_obj_set_style_shadow_width(btn_del_confirm, 0, 0);
    lv_obj_set_style_bg_color(btn_del_confirm, lv_color_hex(0xB71C1C),
                              LV_STATE_PRESSED);
    lv_obj_t *lbl_dd = lv_label_create(btn_del_confirm);
    lv_obj_set_style_text_color(lbl_dd, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(lbl_dd, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_dd, "Delete");
    lv_obj_add_event_cb(btn_del_confirm, on_delete_confirm,
                        LV_EVENT_CLICKED, NULL);

    /* Force initial chip build */
    s_user_chip_count = -1;

    return scr_eq;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh data)
 * ----------------------------------------------------------------------- */

void ui_eq_update(const ui_system_status_t *sys,
                  const ui_eq_presets_data_t *presets)
{
    if (!scr_eq) return;

    s_updating = true;

    /* -- EQ sliders and dB labels -- */
    for (int i = 0; i < UI_EQ_BANDS; i++) {
        lv_slider_set_value(eq_sliders[i], sys->eq_bands[i], LV_ANIM_OFF);

        char db_buf[12];
        int8_t g = sys->eq_bands[i];
        if (g > 0)
            lv_snprintf(db_buf, sizeof(db_buf), "+%d dB", g);
        else
            lv_snprintf(db_buf, sizeof(db_buf), "%d dB", g);
        lv_label_set_text(eq_val_labels[i], db_buf);
    }

    /* -- Built-in preset chips -- */
    for (int i = 0; i < NUM_BUILTIN_PRESETS; i++) {
        bool active = sys->dsp_enabled && sys->dsp_preset &&
                      strcmp(sys->dsp_preset, preset_names[i]) == 0;
        if (active)
            lv_obj_add_state(preset_btns[i], LV_STATE_CHECKED);
        else
            lv_obj_clear_state(preset_btns[i], LV_STATE_CHECKED);
    }

    /* -- User preset chips (rebuild if count changed) -- */
    if (s_user_chip_count != presets->count) {
        rebuild_user_chips(presets, sys->dsp_preset, sys->dsp_enabled);
    } else {
        /* Just update checked states */
        for (int i = 0; i < presets->count && i < UI_MAX_USER_PRESETS; i++) {
            bool active = sys->dsp_enabled && sys->dsp_preset &&
                          strcmp(sys->dsp_preset, s_user_names[i]) == 0;
            if (active)
                lv_obj_add_state(user_chip_objs[i], LV_STATE_CHECKED);
            else
                lv_obj_clear_state(user_chip_objs[i], LV_STATE_CHECKED);
        }
    }

    /* -- DSP toggle -- */
    if (sys->dsp_enabled) {
        lv_obj_add_state(sw_dsp, LV_STATE_CHECKED);
        lv_label_set_text(lbl_dsp_state, "ON");
        lv_obj_set_style_text_color(lbl_dsp_state,
                                    ui_theme_color_accent(), 0);
    } else {
        lv_obj_clear_state(sw_dsp, LV_STATE_CHECKED);
        lv_label_set_text(lbl_dsp_state, "OFF");
        lv_obj_set_style_text_color(lbl_dsp_state,
                                    ui_theme_color_text_secondary(), 0);
    }

    s_updating = false;
}
