/*
 * ui_settings.c — Settings screen for Lyra (720x1280 portrait).
 *
 * Scrollable single-page layout with sections:
 *   - Equalizer (5-band vertical sliders + preset chips + DSP toggle)
 *   - Connectivity (WiFi / Bluetooth status)
 *   - About (device info)
 */

#include "ui_internal.h"
#include <string.h>

/* Layout constants */
#define PAD_SIDE       24
#define CONTENT_W      (UI_HOR_RES - 2 * PAD_SIDE)
#define EQ_SLIDER_H   160     /* Height of vertical EQ sliders */

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_settings;

/* EQ */
static lv_obj_t *eq_sliders[UI_EQ_BANDS];
static lv_obj_t *eq_val_labels[UI_EQ_BANDS];

/* Presets */
static lv_obj_t *preset_btns[4];
static const char *preset_names[] = { "Flat", "Rock", "Jazz", "Bass" };
#define NUM_PRESET_BTNS  4

/* DSP toggle */
static lv_obj_t *sw_dsp;
static lv_obj_t *lbl_dsp_state;

/* Connectivity */
static lv_obj_t *lbl_wifi_value;
static lv_obj_t *lbl_bt_value;

/* Band index stored as user_data for slider events */
static int s_band_idx[UI_EQ_BANDS] = { 0, 1, 2, 3, 4 };

/* Track last DSP state to avoid fighting user slider input */
static bool s_updating = false;

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_eq_slider(lv_event_t *e)
{
    if (s_updating) return;  /* Ignore events triggered by update() */
    int *band = (int *)lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    ui_cmd_set_eq_band(*band, (int8_t)val);
}

static void on_preset_click(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    ui_cmd_set_dsp_preset(name);
}

static void on_dsp_toggle(lv_event_t *e)
{
    (void)e;
    ui_cmd_toggle_dsp();
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Create a section header label ("EQUALIZER", "CONNECTIVITY", etc.) */
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

/* Create a divider line below a section header */
static void create_divider(lv_obj_t *parent)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
    lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
    lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
}

/* Create a key-value info row (e.g. "WiFi" ... "HomeNetwork") */
static lv_obj_t *create_info_row(lv_obj_t *parent, const char *key,
                                  const char *initial_value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, ui_theme_style_setting_row(), 0);
    lv_obj_set_size(row, UI_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl_key = lv_label_create(row);
    lv_obj_add_style(lbl_key, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_key, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_key, key);

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_obj_add_style(lbl_val, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_val, ui_theme_color_text_primary(), 0);
    lv_label_set_text(lbl_val, initial_value);

    return lbl_val;  /* Return value label for dynamic updates */
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_settings_create(void)
{
    scr_settings = lv_obj_create(NULL);
    lv_obj_add_style(scr_settings, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_settings, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_settings);
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

    lv_obj_t *lbl_gear = lv_label_create(header);
    lv_obj_add_style(lbl_gear, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_gear, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_gear, LV_SYMBOL_SETTINGS);

    lv_obj_t *lbl_title = lv_label_create(header);
    lv_obj_add_style(lbl_title, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_title, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_title, "Settings");

    /* ==================================================================
     * SCROLLABLE CONTENT (between header and nav bar)
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

    /* ==================================================================
     * EQUALIZER SECTION
     * ================================================================== */
    create_section(content, "EQUALIZER", 16);
    create_divider(content);

    static const char *eq_freq_labels[UI_EQ_BANDS] = {
        "60Hz", "250Hz", "1kHz", "4kHz", "16kHz"
    };

    /* EQ sliders container */
    lv_obj_t *eq_area = lv_obj_create(content);
    lv_obj_remove_style_all(eq_area);
    lv_obj_set_size(eq_area, CONTENT_W, EQ_SLIDER_H + 60);
    lv_obj_set_style_margin_top(eq_area, 12, 0);
    lv_obj_set_flex_flow(eq_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_area, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(eq_area, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < UI_EQ_BANDS; i++) {
        /* Column per band */
        lv_obj_t *col = lv_obj_create(eq_area);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 80, EQ_SLIDER_H + 60);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_gap(col, 6, 0);

        /* Frequency label on top */
        lv_obj_t *freq_lbl = lv_label_create(col);
        lv_obj_add_style(freq_lbl, ui_theme_style_info(), 0);
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
    }

    /* ==================================================================
     * PRESET CHIPS
     * ================================================================== */
    lv_obj_t *preset_row = lv_obj_create(content);
    lv_obj_remove_style_all(preset_row);
    lv_obj_set_size(preset_row, CONTENT_W, 40);
    lv_obj_set_style_margin_top(preset_row, 8, 0);
    lv_obj_set_flex_flow(preset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preset_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(preset_row, 10, 0);
    lv_obj_set_scrollbar_mode(preset_row, LV_SCROLLBAR_MODE_OFF);

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
     * DSP TOGGLE ROW
     * ================================================================== */
    lv_obj_t *dsp_row = lv_obj_create(content);
    lv_obj_remove_style_all(dsp_row);
    lv_obj_add_style(dsp_row, ui_theme_style_setting_row(), 0);
    lv_obj_set_size(dsp_row, UI_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dsp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dsp_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(dsp_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_margin_top(dsp_row, 8, 0);

    lv_obj_t *dsp_left = lv_obj_create(dsp_row);
    lv_obj_remove_style_all(dsp_left);
    lv_obj_set_size(dsp_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dsp_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(dsp_left, 8, 0);
    lv_obj_set_scrollbar_mode(dsp_left, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl_dsp = lv_label_create(dsp_left);
    lv_obj_add_style(lbl_dsp, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_dsp, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_dsp, "DSP Processing");

    lbl_dsp_state = lv_label_create(dsp_left);
    lv_obj_add_style(lbl_dsp_state, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_dsp_state, ui_theme_color_accent(), 0);
    lv_label_set_text(lbl_dsp_state, "ON");

    sw_dsp = lv_switch_create(dsp_row);
    lv_obj_set_size(sw_dsp, 50, 26);
    lv_obj_set_style_bg_color(sw_dsp, ui_theme_color_slider_track(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw_dsp, ui_theme_color_accent(), (uint32_t)LV_PART_INDICATOR | (uint32_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw_dsp, ui_theme_color_text_primary(), LV_PART_KNOB);
    lv_obj_add_event_cb(sw_dsp, on_dsp_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    /* ==================================================================
     * CONNECTIVITY SECTION
     * ================================================================== */
    create_section(content, "CONNECTIVITY", 24);
    create_divider(content);

    lbl_wifi_value = create_info_row(content, "WiFi", "Not connected");
    lbl_bt_value   = create_info_row(content, "Bluetooth", "Not connected");

    /* ==================================================================
     * ABOUT SECTION
     * ================================================================== */
    create_section(content, "ABOUT", 24);
    create_divider(content);

    create_info_row(content, "Device",   "Lyra DAP");
    create_info_row(content, "DAC",      "ES9039Q2M");
    create_info_row(content, "Firmware", "1.0.0-dev");
    create_info_row(content, "ESP-IDF",  "v5.5.2");

    /* ==================================================================
     * BOTTOM NAVIGATION BAR (56px)
     * ================================================================== */
    ui_create_nav_bar(main, UI_SCREEN_SETTINGS);

    return scr_settings;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh system status)
 * ----------------------------------------------------------------------- */

void ui_settings_update(const ui_system_status_t *sys)
{
    if (!scr_settings) return;

    s_updating = true;  /* Prevent slider events from firing during update */

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

    /* -- Preset chips -- */
    for (int i = 0; i < NUM_PRESET_BTNS; i++) {
        bool active = sys->dsp_enabled && sys->dsp_preset &&
                      strcmp(sys->dsp_preset, preset_names[i]) == 0;
        if (active)
            lv_obj_add_state(preset_btns[i], LV_STATE_CHECKED);
        else
            lv_obj_clear_state(preset_btns[i], LV_STATE_CHECKED);
    }

    /* -- DSP toggle -- */
    if (sys->dsp_enabled) {
        lv_obj_add_state(sw_dsp, LV_STATE_CHECKED);
        lv_label_set_text(lbl_dsp_state, "ON");
    } else {
        lv_obj_clear_state(sw_dsp, LV_STATE_CHECKED);
        lv_label_set_text(lbl_dsp_state, "OFF");
    }

    /* -- WiFi -- */
    if (sys->wifi_connected) {
        char wifi_buf[48];
        lv_snprintf(wifi_buf, sizeof(wifi_buf), "%s  %d dBm",
                    sys->wifi_ssid, sys->wifi_rssi);
        lv_label_set_text(lbl_wifi_value, wifi_buf);
    } else {
        lv_label_set_text(lbl_wifi_value, "Not connected");
    }

    /* -- Bluetooth -- */
    if (sys->bt_connected)
        lv_label_set_text(lbl_bt_value, "Connected");
    else
        lv_label_set_text(lbl_bt_value, "Not connected");

    s_updating = false;
}
