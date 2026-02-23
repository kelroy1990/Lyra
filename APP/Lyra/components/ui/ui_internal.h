#pragma once

/*
 * ui_internal.h — Shared declarations between UI source files.
 * Not part of the public API.
 */

#include "lvgl.h"
#include "ui.h"
#include "ui_data.h"

/* -----------------------------------------------------------------------
 * Theme (ui_theme.c)
 * ----------------------------------------------------------------------- */

void ui_theme_init(void);

/* Style getters */
lv_style_t *ui_theme_style_screen(void);
lv_style_t *ui_theme_style_status_bar(void);
lv_style_t *ui_theme_style_title(void);
lv_style_t *ui_theme_style_subtitle(void);
lv_style_t *ui_theme_style_info(void);
lv_style_t *ui_theme_style_time(void);
lv_style_t *ui_theme_style_btn(void);
lv_style_t *ui_theme_style_btn_pressed(void);
lv_style_t *ui_theme_style_btn_play(void);
lv_style_t *ui_theme_style_btn_play_pressed(void);
lv_style_t *ui_theme_style_slider_main(void);
lv_style_t *ui_theme_style_slider_knob(void);
lv_style_t *ui_theme_style_card(void);
lv_style_t *ui_theme_style_art_placeholder(void);
lv_style_t *ui_theme_style_format_hires(void);
lv_style_t *ui_theme_style_eq_bar(void);
lv_style_t *ui_theme_style_eq_bar_track(void);
lv_style_t *ui_theme_style_preset_chip(void);
lv_style_t *ui_theme_style_preset_chip_active(void);
lv_style_t *ui_theme_style_nav_bar(void);
lv_style_t *ui_theme_style_nav_item(void);
lv_style_t *ui_theme_style_nav_item_active(void);
lv_style_t *ui_theme_style_list_item(void);
lv_style_t *ui_theme_style_list_divider(void);
lv_style_t *ui_theme_style_section_header(void);
lv_style_t *ui_theme_style_setting_row(void);

/* Color getters */
lv_color_t  ui_theme_color_accent(void);
lv_color_t  ui_theme_color_accent_dim(void);
lv_color_t  ui_theme_color_bg(void);
lv_color_t  ui_theme_color_text_primary(void);
lv_color_t  ui_theme_color_text_secondary(void);
lv_color_t  ui_theme_color_slider_track(void);
lv_color_t  ui_theme_color_dsd_badge(void);

/* -----------------------------------------------------------------------
 * Now Playing screen (ui_now_playing.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_now_playing_create(void);
void      ui_now_playing_update(const ui_now_playing_t *np,
                                 const ui_system_status_t *sys);

/* -----------------------------------------------------------------------
 * File Browser screen (ui_browser.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_browser_create(void);
void      ui_browser_update(const ui_browser_data_t *data);

/* -----------------------------------------------------------------------
 * Settings screen (ui_settings.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_settings_create(void);
void      ui_settings_update(const ui_system_status_t *sys);

/* -----------------------------------------------------------------------
 * Shared nav bar (ui_init.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_create_nav_bar(lv_obj_t *parent, ui_screen_t active_screen);

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Format milliseconds as "m:ss" into buf (at least 16 bytes). */
static inline void ui_fmt_time(char *buf, size_t sz, uint32_t ms)
{
    uint32_t sec = ms / 1000;
    uint32_t m   = sec / 60;
    uint32_t s   = sec % 60;
    lv_snprintf(buf, (uint32_t)sz, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
}
