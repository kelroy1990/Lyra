/*
 * ui_theme.c — Lyra UI theme: colors, styles, fonts.
 *
 * Dark audiophile-style theme with warm gold accent on near-black background.
 * All LVGL styles are defined here and exposed via ui_theme_*() getters.
 */

#include "lvgl.h"
#include "ui.h"

/* -----------------------------------------------------------------------
 * Color palette
 * ----------------------------------------------------------------------- */

#define COL_BG_MAIN        lv_color_hex(0x0D0D0D)   /* Near-black background  */
#define COL_BG_CARD        lv_color_hex(0x1A1A1A)   /* Card / panel surface   */
#define COL_BG_ELEVATED    lv_color_hex(0x252525)   /* Elevated surface       */
#define COL_ACCENT         lv_color_hex(0xE0A526)   /* Warm gold accent       */
#define COL_ACCENT_DIM     lv_color_hex(0x8B6718)   /* Dimmed gold            */
#define COL_ACCENT_PRESSED lv_color_hex(0xC48E1F)   /* Darker gold (pressed)  */
#define COL_TEXT_PRIMARY    lv_color_hex(0xFFFFFF)   /* White text             */
#define COL_TEXT_SECONDARY  lv_color_hex(0x999999)   /* Gray text              */
#define COL_TEXT_TERTIARY   lv_color_hex(0x5A5A5A)   /* Dim text               */
#define COL_SLIDER_TRACK    lv_color_hex(0x2A2A2A)   /* Slider / bar track     */
#define COL_DIVIDER         lv_color_hex(0x222222)   /* Separator line         */
#define COL_DSD_BADGE       lv_color_hex(0x4CAF50)   /* DSD green badge        */
#define COL_HIRES_ACCENT    lv_color_hex(0xE0A526)   /* Hi-res gold            */
#define COL_BTN_PRESSED     lv_color_hex(0x333333)   /* Button press feedback  */

/* -----------------------------------------------------------------------
 * Static styles (initialised once in ui_theme_init)
 * ----------------------------------------------------------------------- */

static lv_style_t style_screen;
static lv_style_t style_status_bar;
static lv_style_t style_title;
static lv_style_t style_subtitle;
static lv_style_t style_info;
static lv_style_t style_time;
static lv_style_t style_btn;
static lv_style_t style_btn_pressed;
static lv_style_t style_btn_play;
static lv_style_t style_btn_play_pressed;
static lv_style_t style_slider_main;
static lv_style_t style_slider_knob;
static lv_style_t style_card;
static lv_style_t style_art_placeholder;
static lv_style_t style_format_hires;
static lv_style_t style_eq_bar;
static lv_style_t style_eq_bar_track;
static lv_style_t style_preset_chip;
static lv_style_t style_preset_chip_active;
static lv_style_t style_nav_bar;
static lv_style_t style_nav_item;
static lv_style_t style_nav_item_active;
static lv_style_t style_list_item;
static lv_style_t style_list_divider;
static lv_style_t style_section_header;
static lv_style_t style_setting_row;

static bool s_theme_inited = false;

/* -----------------------------------------------------------------------
 * Init
 * ----------------------------------------------------------------------- */

void ui_theme_init(void)
{
    if (s_theme_inited) return;
    s_theme_inited = true;

    /* Screen background */
    lv_style_init(&style_screen);
    lv_style_set_bg_color(&style_screen, COL_BG_MAIN);
    lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
    lv_style_set_pad_all(&style_screen, 0);
    lv_style_set_border_width(&style_screen, 0);
    lv_style_set_radius(&style_screen, 0);

    /* Status bar */
    lv_style_init(&style_status_bar);
    lv_style_set_bg_color(&style_status_bar, COL_BG_CARD);
    lv_style_set_bg_opa(&style_status_bar, LV_OPA_COVER);
    lv_style_set_pad_hor(&style_status_bar, 24);
    lv_style_set_pad_ver(&style_status_bar, 10);
    lv_style_set_border_width(&style_status_bar, 0);
    lv_style_set_radius(&style_status_bar, 0);

    /* Track title (large white, bold feel) */
    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, COL_TEXT_PRIMARY);
    lv_style_set_text_font(&style_title, &lv_font_montserrat_28);

    /* Subtitle / artist (medium gray) */
    lv_style_init(&style_subtitle);
    lv_style_set_text_color(&style_subtitle, COL_TEXT_SECONDARY);
    lv_style_set_text_font(&style_subtitle, &lv_font_montserrat_20);

    /* Info line (small, dim) */
    lv_style_init(&style_info);
    lv_style_set_text_color(&style_info, COL_TEXT_TERTIARY);
    lv_style_set_text_font(&style_info, &lv_font_montserrat_14);

    /* Time labels (small, lighter gray) */
    lv_style_init(&style_time);
    lv_style_set_text_color(&style_time, COL_TEXT_SECONDARY);
    lv_style_set_text_font(&style_time, &lv_font_montserrat_14);

    /* Transport buttons (prev/next — transparent, white icons) */
    lv_style_init(&style_btn);
    lv_style_set_bg_opa(&style_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_btn, COL_TEXT_PRIMARY);
    lv_style_set_text_font(&style_btn, &lv_font_montserrat_20);
    lv_style_set_border_width(&style_btn, 0);
    lv_style_set_shadow_width(&style_btn, 0);
    lv_style_set_pad_all(&style_btn, 20);

    /* Transport buttons — pressed state */
    lv_style_init(&style_btn_pressed);
    lv_style_set_bg_color(&style_btn_pressed, COL_BTN_PRESSED);
    lv_style_set_bg_opa(&style_btn_pressed, LV_OPA_COVER);
    lv_style_set_radius(&style_btn_pressed, LV_RADIUS_CIRCLE);

    /* Play/Pause button — circular gold background */
    lv_style_init(&style_btn_play);
    lv_style_set_bg_color(&style_btn_play, COL_ACCENT);
    lv_style_set_bg_opa(&style_btn_play, LV_OPA_COVER);
    lv_style_set_text_color(&style_btn_play, COL_BG_MAIN);
    lv_style_set_text_font(&style_btn_play, &lv_font_montserrat_28);
    lv_style_set_border_width(&style_btn_play, 0);
    lv_style_set_shadow_width(&style_btn_play, 0);
    lv_style_set_radius(&style_btn_play, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&style_btn_play, 20);
    lv_style_set_min_width(&style_btn_play, 72);
    lv_style_set_min_height(&style_btn_play, 72);

    /* Play button — pressed state */
    lv_style_init(&style_btn_play_pressed);
    lv_style_set_bg_color(&style_btn_play_pressed, COL_ACCENT_PRESSED);

    /* Slider main indicator */
    lv_style_init(&style_slider_main);
    lv_style_set_bg_color(&style_slider_main, COL_ACCENT);
    lv_style_set_bg_opa(&style_slider_main, LV_OPA_COVER);
    lv_style_set_radius(&style_slider_main, 4);

    /* Slider knob */
    lv_style_init(&style_slider_knob);
    lv_style_set_bg_color(&style_slider_knob, COL_ACCENT);
    lv_style_set_bg_opa(&style_slider_knob, LV_OPA_COVER);
    lv_style_set_pad_all(&style_slider_knob, 7);
    lv_style_set_radius(&style_slider_knob, LV_RADIUS_CIRCLE);

    /* Card surface */
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, COL_BG_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 16);
    lv_style_set_pad_all(&style_card, 20);
    lv_style_set_border_width(&style_card, 0);

    /* Album art placeholder */
    lv_style_init(&style_art_placeholder);
    lv_style_set_bg_color(&style_art_placeholder, COL_BG_ELEVATED);
    lv_style_set_bg_opa(&style_art_placeholder, LV_OPA_COVER);
    lv_style_set_radius(&style_art_placeholder, 20);
    lv_style_set_border_width(&style_art_placeholder, 1);
    lv_style_set_border_color(&style_art_placeholder, COL_DIVIDER);

    /* Hi-res format badge */
    lv_style_init(&style_format_hires);
    lv_style_set_text_color(&style_format_hires, COL_HIRES_ACCENT);
    lv_style_set_text_font(&style_format_hires, &lv_font_montserrat_14);

    /* EQ bar (vertical bar for each band) */
    lv_style_init(&style_eq_bar);
    lv_style_set_bg_color(&style_eq_bar, COL_ACCENT);
    lv_style_set_bg_opa(&style_eq_bar, LV_OPA_COVER);
    lv_style_set_radius(&style_eq_bar, 3);

    /* EQ bar track (background) */
    lv_style_init(&style_eq_bar_track);
    lv_style_set_bg_color(&style_eq_bar_track, COL_SLIDER_TRACK);
    lv_style_set_bg_opa(&style_eq_bar_track, LV_OPA_COVER);
    lv_style_set_radius(&style_eq_bar_track, 3);

    /* Preset chip (small selectable button) */
    lv_style_init(&style_preset_chip);
    lv_style_set_bg_color(&style_preset_chip, COL_BG_ELEVATED);
    lv_style_set_bg_opa(&style_preset_chip, LV_OPA_COVER);
    lv_style_set_text_color(&style_preset_chip, COL_TEXT_SECONDARY);
    lv_style_set_text_font(&style_preset_chip, &lv_font_montserrat_14);
    lv_style_set_radius(&style_preset_chip, 16);
    lv_style_set_pad_hor(&style_preset_chip, 16);
    lv_style_set_pad_ver(&style_preset_chip, 6);
    lv_style_set_border_width(&style_preset_chip, 0);
    lv_style_set_shadow_width(&style_preset_chip, 0);

    /* Preset chip — active/selected */
    lv_style_init(&style_preset_chip_active);
    lv_style_set_bg_color(&style_preset_chip_active, COL_ACCENT);
    lv_style_set_text_color(&style_preset_chip_active, COL_BG_MAIN);

    /* Navigation bar */
    lv_style_init(&style_nav_bar);
    lv_style_set_bg_color(&style_nav_bar, COL_BG_CARD);
    lv_style_set_bg_opa(&style_nav_bar, LV_OPA_COVER);
    lv_style_set_border_side(&style_nav_bar, LV_BORDER_SIDE_TOP);
    lv_style_set_border_width(&style_nav_bar, 1);
    lv_style_set_border_color(&style_nav_bar, COL_DIVIDER);
    lv_style_set_radius(&style_nav_bar, 0);
    lv_style_set_pad_ver(&style_nav_bar, 8);
    lv_style_set_pad_hor(&style_nav_bar, 0);

    /* Nav item (inactive) */
    lv_style_init(&style_nav_item);
    lv_style_set_bg_opa(&style_nav_item, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_nav_item, COL_TEXT_TERTIARY);
    lv_style_set_text_font(&style_nav_item, &lv_font_montserrat_14);
    lv_style_set_border_width(&style_nav_item, 0);
    lv_style_set_shadow_width(&style_nav_item, 0);
    lv_style_set_pad_all(&style_nav_item, 8);

    /* Nav item (active) */
    lv_style_init(&style_nav_item_active);
    lv_style_set_text_color(&style_nav_item_active, COL_ACCENT);

    /* File list item row */
    lv_style_init(&style_list_item);
    lv_style_set_bg_opa(&style_list_item, LV_OPA_TRANSP);
    lv_style_set_pad_hor(&style_list_item, 24);
    lv_style_set_pad_ver(&style_list_item, 14);
    lv_style_set_border_width(&style_list_item, 0);
    lv_style_set_shadow_width(&style_list_item, 0);
    lv_style_set_radius(&style_list_item, 0);
    lv_style_set_text_color(&style_list_item, COL_TEXT_PRIMARY);
    lv_style_set_text_font(&style_list_item, &lv_font_montserrat_16);

    /* File list divider line */
    lv_style_init(&style_list_divider);
    lv_style_set_bg_color(&style_list_divider, COL_DIVIDER);
    lv_style_set_bg_opa(&style_list_divider, LV_OPA_COVER);
    lv_style_set_radius(&style_list_divider, 0);
    lv_style_set_border_width(&style_list_divider, 0);

    /* Section header (gold label, e.g. "EQUALIZER", "CONNECTIVITY") */
    lv_style_init(&style_section_header);
    lv_style_set_text_color(&style_section_header, COL_ACCENT);
    lv_style_set_text_font(&style_section_header, &lv_font_montserrat_14);
    lv_style_set_pad_left(&style_section_header, 24);
    lv_style_set_pad_top(&style_section_header, 4);
    lv_style_set_pad_bottom(&style_section_header, 8);

    /* Setting row (key-value info row) */
    lv_style_init(&style_setting_row);
    lv_style_set_bg_opa(&style_setting_row, LV_OPA_TRANSP);
    lv_style_set_pad_hor(&style_setting_row, 24);
    lv_style_set_pad_ver(&style_setting_row, 10);
    lv_style_set_border_width(&style_setting_row, 0);
    lv_style_set_shadow_width(&style_setting_row, 0);
    lv_style_set_radius(&style_setting_row, 0);
}

/* -----------------------------------------------------------------------
 * Style getters
 * ----------------------------------------------------------------------- */

lv_style_t *ui_theme_style_screen(void)           { return &style_screen; }
lv_style_t *ui_theme_style_status_bar(void)        { return &style_status_bar; }
lv_style_t *ui_theme_style_title(void)             { return &style_title; }
lv_style_t *ui_theme_style_subtitle(void)          { return &style_subtitle; }
lv_style_t *ui_theme_style_info(void)              { return &style_info; }
lv_style_t *ui_theme_style_time(void)              { return &style_time; }
lv_style_t *ui_theme_style_btn(void)               { return &style_btn; }
lv_style_t *ui_theme_style_btn_pressed(void)       { return &style_btn_pressed; }
lv_style_t *ui_theme_style_btn_play(void)          { return &style_btn_play; }
lv_style_t *ui_theme_style_btn_play_pressed(void)  { return &style_btn_play_pressed; }
lv_style_t *ui_theme_style_slider_main(void)       { return &style_slider_main; }
lv_style_t *ui_theme_style_slider_knob(void)       { return &style_slider_knob; }
lv_style_t *ui_theme_style_card(void)              { return &style_card; }
lv_style_t *ui_theme_style_art_placeholder(void)   { return &style_art_placeholder; }
lv_style_t *ui_theme_style_format_hires(void)      { return &style_format_hires; }
lv_style_t *ui_theme_style_eq_bar(void)            { return &style_eq_bar; }
lv_style_t *ui_theme_style_eq_bar_track(void)      { return &style_eq_bar_track; }
lv_style_t *ui_theme_style_preset_chip(void)       { return &style_preset_chip; }
lv_style_t *ui_theme_style_preset_chip_active(void){ return &style_preset_chip_active; }
lv_style_t *ui_theme_style_nav_bar(void)           { return &style_nav_bar; }
lv_style_t *ui_theme_style_nav_item(void)          { return &style_nav_item; }
lv_style_t *ui_theme_style_nav_item_active(void)   { return &style_nav_item_active; }
lv_style_t *ui_theme_style_list_item(void)         { return &style_list_item; }
lv_style_t *ui_theme_style_list_divider(void)      { return &style_list_divider; }
lv_style_t *ui_theme_style_section_header(void)    { return &style_section_header; }
lv_style_t *ui_theme_style_setting_row(void)       { return &style_setting_row; }

lv_color_t  ui_theme_color_accent(void)            { return COL_ACCENT; }
lv_color_t  ui_theme_color_accent_dim(void)        { return COL_ACCENT_DIM; }
lv_color_t  ui_theme_color_bg(void)                { return COL_BG_MAIN; }
lv_color_t  ui_theme_color_text_primary(void)      { return COL_TEXT_PRIMARY; }
lv_color_t  ui_theme_color_text_secondary(void)    { return COL_TEXT_SECONDARY; }
lv_color_t  ui_theme_color_slider_track(void)      { return COL_SLIDER_TRACK; }
lv_color_t  ui_theme_color_dsd_badge(void)         { return COL_DSD_BADGE; }
