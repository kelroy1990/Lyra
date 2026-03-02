/*
 * ui_about.c -- About / System Info sub-screen for Lyra (720x1280 portrait).
 *
 * Layout (top to bottom):
 *
 *   +----------------------------------+
 *   |  <  About                        |  Header (48px)
 *   +----------------------------------+
 *   |                                  |
 *   |  DEVICE                          |  Scrollable content
 *   |  Model           Lyra DAP        |
 *   |  DAC             ES9039Q2M       |
 *   |  Display         720 x 1280      |
 *   |                                  |
 *   |  FIRMWARE                        |
 *   |  ...                             |
 *   |                                  |
 *   |  HARDWARE / STORAGE / ...        |
 *   |                                  |
 *   +----------------------------------+
 */

#include "ui_internal.h"
#include <stdio.h>
#include <string.h>

/* Layout constants */
#define PAD_SIDE  24

/* -----------------------------------------------------------------------
 * Widget references (dynamic labels updated periodically)
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_about;

static lv_obj_t *lbl_ram_value;
static lv_obj_t *lbl_psram_value;
static lv_obj_t *lbl_sd_used;
static lv_obj_t *lbl_sd_free;
static lv_obj_t *lbl_current_file;
static lv_obj_t *lbl_ip_address;
static lv_obj_t *lbl_uptime;
static lv_obj_t *lbl_cpu_temp;
static lv_obj_t *lbl_audio_source;

/* -----------------------------------------------------------------------
 * Local helpers (same pattern as ui_settings.c, self-contained)
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

    return lbl_val;
}

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_navigate_to(UI_SCREEN_SETTINGS);
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_about_create(void)
{
    scr_about = lv_obj_create(NULL);
    lv_obj_add_style(scr_about, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_about, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_about);
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
    lv_obj_set_style_pad_gap(header, 8, 0);

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
    lv_obj_add_style(lbl_title, ui_theme_style_subtitle(), 0);
    lv_obj_set_style_text_color(lbl_title, ui_theme_color_text_primary(), 0);
    lv_label_set_text(lbl_title, "About");

    /* ==================================================================
     * SCROLLABLE CONTENT
     * ================================================================== */
    lv_obj_t *content = lv_obj_create(main);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, UI_HOR_RES);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(content, 0, 0);
    lv_obj_set_style_pad_bottom(content, 32, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* Themed scrollbar */
    lv_obj_set_style_bg_color(content, lv_color_hex(0x333333), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, (lv_opa_t)120, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(content, 2, LV_PART_SCROLLBAR);

    /* ==================================================================
     * DEVICE SECTION
     * ================================================================== */
    create_section(content, "DEVICE", 16);
    create_divider(content);
    create_info_row(content, "Model", "Lyra DAP");
    create_info_row(content, "DAC", "ES9039Q2M");
    create_info_row(content, "Display", "720 x 1280");

    /* ==================================================================
     * FIRMWARE SECTION
     * ================================================================== */
    create_section(content, "FIRMWARE", 24);
    create_divider(content);
    create_info_row(content, "Version", "1.0.0-dev");
    create_info_row(content, "Build Date", __DATE__);
    create_info_row(content, "ESP-IDF", "v5.5.2");

    /* ==================================================================
     * HARDWARE SECTION
     * ================================================================== */
    create_section(content, "HARDWARE", 24);
    create_divider(content);
    create_info_row(content, "CPU", "ESP32-P4 RISC-V 400MHz");
    lbl_ram_value   = create_info_row(content, "Internal RAM", "312 / 768 KB");
    lbl_psram_value = create_info_row(content, "PSRAM", "8.0 / 32.0 MB");
    create_info_row(content, "Flash", "32 MB");

    /* ==================================================================
     * STORAGE SECTION
     * ================================================================== */
    create_section(content, "STORAGE", 24);
    create_divider(content);
    create_info_row(content, "SD Card", "64 GB");
    lbl_sd_used     = create_info_row(content, "Used", "23.4 GB");
    lbl_sd_free     = create_info_row(content, "Free", "40.6 GB");
    create_info_row(content, "Filesystem", "exFAT");
    lbl_current_file = create_info_row(content, "Playing", "---");

    /* ==================================================================
     * CONNECTIVITY SECTION
     * ================================================================== */
    create_section(content, "CONNECTIVITY", 24);
    create_divider(content);
    create_info_row(content, "WiFi MAC", "A4:CF:12:E8:3B:7D");
    create_info_row(content, "BT MAC", "A4:CF:12:E8:3B:7E");
    lbl_ip_address = create_info_row(content, "IP Address", "N/A");

    /* ==================================================================
     * SYSTEM SECTION
     * ================================================================== */
    create_section(content, "SYSTEM", 24);
    create_divider(content);
    lbl_uptime       = create_info_row(content, "Uptime", "0h 0m 0s");
    lbl_cpu_temp     = create_info_row(content, "Temperature", "47 C");
    lbl_audio_source = create_info_row(content, "Audio Source", "SD Card");

    return scr_about;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh device info)
 * ----------------------------------------------------------------------- */

void ui_about_update(const ui_device_info_t *info)
{
    if (!scr_about) return;

    /* RAM */
    char buf[64];
    lv_snprintf(buf, sizeof(buf), "%lu / %lu KB",
                (unsigned long)info->internal_ram_used_kb,
                (unsigned long)info->internal_ram_total_kb);
    lv_label_set_text(lbl_ram_value, buf);

    /* PSRAM (show as MB) */
    lv_snprintf(buf, sizeof(buf), "%.1f / %.1f MB",
                (double)info->psram_used_kb / 1024.0,
                (double)info->psram_total_kb / 1024.0);
    lv_label_set_text(lbl_psram_value, buf);

    /* Storage */
    lv_label_set_text(lbl_sd_used, info->sd_used);
    lv_label_set_text(lbl_sd_free, info->sd_free);
    lv_label_set_text(lbl_current_file, info->current_file[0] ? info->current_file : "---");

    /* IP */
    lv_label_set_text(lbl_ip_address, info->ip_address);

    /* Uptime */
    uint32_t s = info->uptime_seconds;
    uint32_t h = s / 3600;
    uint32_t m = (s % 3600) / 60;
    uint32_t sec = s % 60;
    lv_snprintf(buf, sizeof(buf), "%luh %lum %lus",
                (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    lv_label_set_text(lbl_uptime, buf);

    /* Temperature */
    if (info->cpu_temp_c != -128)
        lv_snprintf(buf, sizeof(buf), "%d C", info->cpu_temp_c);
    else
        lv_snprintf(buf, sizeof(buf), "N/A");
    lv_label_set_text(lbl_cpu_temp, buf);

    /* Audio source */
    lv_label_set_text(lbl_audio_source, info->audio_source);
}
