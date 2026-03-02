/*
 * ui_usb_dac.c — USB DAC sub-screen for Lyra.
 *
 * Layout (top to bottom):
 *
 *   ┌──────────────────────────────────┐
 *   │  ◀  USB DAC                      │  Header (48px)
 *   ├──────────────────────────────────┤
 *   │                                  │
 *   │  STATUS                          │  Section
 *   │  ┌────────────────────────────┐  │
 *   │  │  🔌 USB Audio              │  │  Title (20pt)
 *   │  │  ● Streaming               │  │  State + dot
 *   │  │                            │  │
 *   │  │  PCM 384 kHz / 32-bit     │  │  Format (28pt, gold)
 *   │  └────────────────────────────┘  │
 *   │                                  │
 *   │  SIGNAL LEVEL                    │  Section (streaming only)
 *   │  ┌────────────────────────────┐  │
 *   │  │  L  ██████████░░  -12 dB  │  │  Level bar + dBFS
 *   │  │  R  █████████░░░  -14 dB  │  │
 *   │  └────────────────────────────┘  │
 *   │                                  │
 *   │  USB DAC MODE                    │  Section
 *   │  ┌────────────────────────────┐  │
 *   │  │  Enable USB DAC   [====]  │  │  Toggle
 *   │  │                            │  │
 *   │  │  When enabled, Lyra acts   │  │  Description
 *   │  │  as a USB audio interface  │  │
 *   │  │  for your computer.        │  │
 *   │  └────────────────────────────┘  │
 *   │                                  │
 *   └──────────────────────────────────┘
 */

#include "ui_internal.h"
#include <string.h>

/* Layout constants */
#define PAD_SIDE  24
#define CONTENT_W (UI_HOR_RES - 2 * PAD_SIDE)

/* State colors */
#define COL_STREAMING   lv_color_hex(0x4CAF50)   /* green  */
#define COL_CONNECTED   lv_color_hex(0xFFA726)   /* amber  */
#define COL_DISCONNECTED lv_color_hex(0x999999)  /* gray   */

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_usb_dac;

/* Status card */
static lv_obj_t *card_status;
static lv_obj_t *dot_usb_state;
static lv_obj_t *lbl_usb_state;
static lv_obj_t *lbl_usb_format;

/* Signal level section */
static lv_obj_t *lbl_signal_section;
static lv_obj_t *card_signal;
static lv_obj_t *bar_level_l;
static lv_obj_t *bar_level_r;
static lv_obj_t *lbl_level_l;
static lv_obj_t *lbl_level_r;

/* Enable toggle */
static lv_obj_t *sw_usb_enable;

/* Previous state for toast wiring */
static ui_usb_dac_state_t s_prev_state = UI_USB_DAC_DISCONNECTED;
static bool s_toast_seeded = false;

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_navigate_to(UI_SCREEN_NOW_PLAYING);
}

static void on_enable_toggle(lv_event_t *e)
{
    (void)e;
    bool checked = lv_obj_has_state(sw_usb_enable, LV_STATE_CHECKED);
    ui_cmd_usb_dac_enable(checked);
}

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
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_usb_dac_create(void)
{
    scr_usb_dac = lv_obj_create(NULL);
    lv_obj_add_style(scr_usb_dac, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_usb_dac, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_usb_dac);
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
    lv_label_set_text(lbl_title, "USB DAC");

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
     * STATUS section
     * ================================================================== */
    create_section(content, "STATUS", 16);

    card_status = lv_obj_create(content);
    lv_obj_remove_style_all(card_status);
    lv_obj_add_style(card_status, ui_theme_style_card(), 0);
    lv_obj_set_size(card_status, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(card_status, 8, 0);
    lv_obj_set_flex_flow(card_status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_status, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card_status, 8, 0);
    lv_obj_set_scrollbar_mode(card_status, LV_SCROLLBAR_MODE_OFF);

    /* USB Audio title with icon */
    lv_obj_t *lbl_usb_title = lv_label_create(card_status);
    lv_obj_add_style(lbl_usb_title, ui_theme_style_subtitle(), 0);
    lv_label_set_text(lbl_usb_title, LV_SYMBOL_USB "  USB Audio");

    /* State line (colored dot + text) */
    lv_obj_t *state_row = lv_obj_create(card_status);
    lv_obj_remove_style_all(state_row);
    lv_obj_set_size(state_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(state_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(state_row, 8, 0);
    lv_obj_set_scrollbar_mode(state_row, LV_SCROLLBAR_MODE_OFF);

    dot_usb_state = lv_obj_create(state_row);
    lv_obj_remove_style_all(dot_usb_state);
    lv_obj_set_size(dot_usb_state, 10, 10);
    lv_obj_set_style_radius(dot_usb_state, 5, 0);
    lv_obj_set_style_bg_color(dot_usb_state, COL_DISCONNECTED, 0);
    lv_obj_set_style_bg_opa(dot_usb_state, LV_OPA_COVER, 0);

    lbl_usb_state = lv_label_create(state_row);
    lv_obj_add_style(lbl_usb_state, ui_theme_style_info(), 0);
    lv_label_set_text(lbl_usb_state, "Disconnected");
    lv_obj_set_style_text_color(lbl_usb_state, COL_DISCONNECTED, 0);

    /* Format display (large, gold when hi-res) */
    lbl_usb_format = lv_label_create(card_status);
    lv_obj_add_style(lbl_usb_format, ui_theme_style_title(), 0);
    lv_label_set_text(lbl_usb_format, "---");
    lv_obj_set_style_text_color(lbl_usb_format,
                                ui_theme_color_text_secondary(), 0);
    lv_obj_set_style_margin_top(lbl_usb_format, 8, 0);

    /* ==================================================================
     * SIGNAL LEVEL section (hidden when not streaming)
     * ================================================================== */
    lbl_signal_section = create_section(content, "SIGNAL LEVEL", 24);

    card_signal = lv_obj_create(content);
    lv_obj_remove_style_all(card_signal);
    lv_obj_add_style(card_signal, ui_theme_style_card(), 0);
    lv_obj_set_size(card_signal, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(card_signal, 8, 0);
    lv_obj_set_flex_flow(card_signal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_signal, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card_signal, 12, 0);
    lv_obj_set_scrollbar_mode(card_signal, LV_SCROLLBAR_MODE_OFF);

    /* Left channel row */
    lv_obj_t *row_l = lv_obj_create(card_signal);
    lv_obj_remove_style_all(row_l);
    lv_obj_set_size(row_l, CONTENT_W - 40, 28);
    lv_obj_set_flex_flow(row_l, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_l, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_l, 10, 0);
    lv_obj_set_scrollbar_mode(row_l, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl_l = lv_label_create(row_l);
    lv_obj_add_style(lbl_l, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_l, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_l, "L");
    lv_obj_set_width(lbl_l, 20);

    bar_level_l = lv_bar_create(row_l);
    lv_obj_set_flex_grow(bar_level_l, 1);
    lv_obj_set_height(bar_level_l, 12);
    lv_bar_set_range(bar_level_l, -60, 0);
    lv_bar_set_value(bar_level_l, -60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_level_l, ui_theme_color_slider_track(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_level_l, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_level_l, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_level_l, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_level_l, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_level_l, 3, LV_PART_INDICATOR);

    lbl_level_l = lv_label_create(row_l);
    lv_obj_add_style(lbl_level_l, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_level_l, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_level_l, "--- dB");
    lv_obj_set_width(lbl_level_l, 70);

    /* Right channel row */
    lv_obj_t *row_r = lv_obj_create(card_signal);
    lv_obj_remove_style_all(row_r);
    lv_obj_set_size(row_r, CONTENT_W - 40, 28);
    lv_obj_set_flex_flow(row_r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_r, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_r, 10, 0);
    lv_obj_set_scrollbar_mode(row_r, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl_r = lv_label_create(row_r);
    lv_obj_add_style(lbl_r, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_r, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_r, "R");
    lv_obj_set_width(lbl_r, 20);

    bar_level_r = lv_bar_create(row_r);
    lv_obj_set_flex_grow(bar_level_r, 1);
    lv_obj_set_height(bar_level_r, 12);
    lv_bar_set_range(bar_level_r, -60, 0);
    lv_bar_set_value(bar_level_r, -60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_level_r, ui_theme_color_slider_track(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_level_r, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_level_r, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_level_r, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_level_r, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_level_r, 3, LV_PART_INDICATOR);

    lbl_level_r = lv_label_create(row_r);
    lv_obj_add_style(lbl_level_r, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_level_r, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_level_r, "--- dB");
    lv_obj_set_width(lbl_level_r, 70);

    /* Start signal section hidden */
    lv_obj_add_flag(lbl_signal_section, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card_signal, LV_OBJ_FLAG_HIDDEN);

    /* ==================================================================
     * USB DAC MODE section
     * ================================================================== */
    create_section(content, "USB DAC MODE", 24);

    lv_obj_t *card_mode = lv_obj_create(content);
    lv_obj_remove_style_all(card_mode);
    lv_obj_add_style(card_mode, ui_theme_style_card(), 0);
    lv_obj_set_size(card_mode, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(card_mode, 8, 0);
    lv_obj_set_flex_flow(card_mode, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_mode, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card_mode, 12, 0);
    lv_obj_set_scrollbar_mode(card_mode, LV_SCROLLBAR_MODE_OFF);

    /* Toggle row */
    lv_obj_t *toggle_row = lv_obj_create(card_mode);
    lv_obj_remove_style_all(toggle_row);
    lv_obj_set_size(toggle_row, CONTENT_W - 40, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(toggle_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *lbl_enable = lv_label_create(toggle_row);
    lv_obj_add_style(lbl_enable, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_enable, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(lbl_enable, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_enable, "Enable USB DAC");

    sw_usb_enable = lv_switch_create(toggle_row);
    lv_obj_set_size(sw_usb_enable, 50, 26);
    lv_obj_set_style_bg_color(sw_usb_enable, ui_theme_color_slider_track(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw_usb_enable, ui_theme_color_accent(),
                              (uint32_t)LV_PART_INDICATOR | (uint32_t)LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw_usb_enable, ui_theme_color_text_primary(),
                              LV_PART_KNOB);
    lv_obj_add_event_cb(sw_usb_enable, on_enable_toggle,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Description text */
    lv_obj_t *lbl_desc = lv_label_create(card_mode);
    lv_obj_add_style(lbl_desc, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_desc, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_desc,
        "When enabled, Lyra acts as a high-resolution\n"
        "USB audio interface for your computer or phone.\n"
        "Supports PCM up to 768 kHz / 32-bit and DSD512.");
    lv_obj_set_width(lbl_desc, CONTENT_W - 40);

    /* Init tracking */
    s_prev_state = UI_USB_DAC_DISCONNECTED;
    s_toast_seeded = false;

    return scr_usb_dac;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh data)
 * ----------------------------------------------------------------------- */

void ui_usb_dac_update(const ui_usb_dac_data_t *data)
{
    if (!scr_usb_dac) return;

    /* -- State text with colored dot -- */
    lv_color_t state_col;
    const char *state_text;

    switch (data->state) {
        case UI_USB_DAC_STREAMING:
            state_text = "Streaming";
            state_col = COL_STREAMING;
            break;
        case UI_USB_DAC_CONNECTED:
            state_text = "Connected";
            state_col = COL_CONNECTED;
            break;
        default:
            state_text = "Disconnected";
            state_col = COL_DISCONNECTED;
            break;
    }
    lv_label_set_text(lbl_usb_state, state_text);
    lv_obj_set_style_text_color(lbl_usb_state, state_col, 0);
    lv_obj_set_style_bg_color(dot_usb_state, state_col, 0);

    /* -- Format display -- */
    if (data->state == UI_USB_DAC_STREAMING && data->format_str[0]) {
        lv_label_set_text(lbl_usb_format, data->format_str);
        /* Gold for hi-res (>48kHz or >16-bit or DSD) */
        bool hires = (data->sample_rate > 48000 ||
                      data->bits_per_sample > 16 || data->is_dsd);
        lv_obj_set_style_text_color(lbl_usb_format,
            hires ? ui_theme_color_accent()
                  : ui_theme_color_text_secondary(), 0);
    } else if (data->state == UI_USB_DAC_CONNECTED) {
        lv_label_set_text(lbl_usb_format, "Waiting for signal...");
        lv_obj_set_style_text_color(lbl_usb_format,
                                    ui_theme_color_text_secondary(), 0);
    } else {
        lv_label_set_text(lbl_usb_format, "---");
        lv_obj_set_style_text_color(lbl_usb_format,
                                    ui_theme_color_text_secondary(), 0);
    }

    /* -- Signal level section visibility -- */
    if (data->state == UI_USB_DAC_STREAMING) {
        lv_obj_clear_flag(lbl_signal_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(card_signal, LV_OBJ_FLAG_HIDDEN);

        /* Update level bars (clamp to range) */
        int32_t l_val = (int32_t)data->level_db_l;
        int32_t r_val = (int32_t)data->level_db_r;
        if (l_val < -60) l_val = -60;
        if (l_val > 0) l_val = 0;
        if (r_val < -60) r_val = -60;
        if (r_val > 0) r_val = 0;

        lv_bar_set_value(bar_level_l, l_val, LV_ANIM_ON);
        lv_bar_set_value(bar_level_r, r_val, LV_ANIM_ON);

        /* Color bars: green normally, amber > -12, red > -3 */
        lv_color_t col_l = lv_color_hex(0x4CAF50);
        lv_color_t col_r = lv_color_hex(0x4CAF50);
        if (l_val > -3) col_l = lv_color_hex(0xFF5252);
        else if (l_val > -12) col_l = lv_color_hex(0xFFA726);
        if (r_val > -3) col_r = lv_color_hex(0xFF5252);
        else if (r_val > -12) col_r = lv_color_hex(0xFFA726);

        lv_obj_set_style_bg_color(bar_level_l, col_l, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar_level_r, col_r, LV_PART_INDICATOR);

        /* dBFS text */
        char db_buf[16];
        lv_snprintf(db_buf, sizeof(db_buf), "%.0f dB", (double)data->level_db_l);
        lv_label_set_text(lbl_level_l, db_buf);
        lv_snprintf(db_buf, sizeof(db_buf), "%.0f dB", (double)data->level_db_r);
        lv_label_set_text(lbl_level_r, db_buf);
    } else {
        lv_obj_add_flag(lbl_signal_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_signal, LV_OBJ_FLAG_HIDDEN);
    }

    /* -- Sync toggle switch state -- */
    if (data->enabled)
        lv_obj_add_state(sw_usb_enable, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(sw_usb_enable, LV_STATE_CHECKED);

    /* -- Toast wiring -- */
    if (!s_toast_seeded) {
        s_toast_seeded = true;
        s_prev_state = data->state;
    } else if (data->state != s_prev_state) {
        if (data->state == UI_USB_DAC_STREAMING) {
            ui_toast_show(UI_TOAST_SUCCESS, "USB DAC streaming", 3000);
        } else if (data->state == UI_USB_DAC_CONNECTED &&
                   s_prev_state == UI_USB_DAC_DISCONNECTED) {
            ui_toast_show(UI_TOAST_INFO, "USB DAC connected", 2500);
        } else if (data->state == UI_USB_DAC_DISCONNECTED &&
                   s_prev_state != UI_USB_DAC_DISCONNECTED) {
            ui_toast_show(UI_TOAST_INFO, "USB DAC disabled", 2500);
        }
        s_prev_state = data->state;
    }
}
