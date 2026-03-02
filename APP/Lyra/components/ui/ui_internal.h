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
 * WiFi sub-screen (ui_wifi.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_wifi_create(void);
void      ui_wifi_update(const ui_wifi_scan_data_t *data);

/* -----------------------------------------------------------------------
 * Net Audio sub-screen (ui_net_audio.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_net_audio_create(void);
void      ui_net_audio_update(const ui_net_audio_data_t *data);

/* -----------------------------------------------------------------------
 * USB DAC sub-screen (ui_usb_dac.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_usb_dac_create(void);
void      ui_usb_dac_update(const ui_usb_dac_data_t *data);

/* -----------------------------------------------------------------------
 * EQ sub-screen (ui_eq.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_eq_create(void);
void      ui_eq_update(const ui_system_status_t *sys,
                        const ui_eq_presets_data_t *presets);

/* -----------------------------------------------------------------------
 * Queue sub-screen (ui_queue.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_queue_create(void);
void      ui_queue_update(const ui_queue_data_t *queue,
                           const ui_now_playing_t *np);

/* -----------------------------------------------------------------------
 * About sub-screen (ui_about.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_about_create(void);
void      ui_about_update(const ui_device_info_t *info);

/* -----------------------------------------------------------------------
 * Qobuz sub-screen (ui_qobuz.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_qobuz_create(void);
void      ui_qobuz_update(const ui_qobuz_data_t *data);

/* -----------------------------------------------------------------------
 * Subsonic sub-screen (ui_subsonic.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_subsonic_create(void);
void      ui_subsonic_update(const ui_subsonic_data_t *data);

/* -----------------------------------------------------------------------
 * Lock screen (ui_lock.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_lock_create(void);
void      ui_lock_update(const ui_now_playing_t *np,
                          const ui_system_status_t *sys);

/* -----------------------------------------------------------------------
 * Shared nav bar (ui_init.c)
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_create_nav_bar(lv_obj_t *parent, ui_screen_t active_screen);

/* -----------------------------------------------------------------------
 * Toast notifications (ui_toast.c)
 * ----------------------------------------------------------------------- */

typedef enum {
    UI_TOAST_INFO,
    UI_TOAST_SUCCESS,
    UI_TOAST_WARNING,
    UI_TOAST_ERROR,
} ui_toast_type_t;

/* Show a toast on lv_layer_top(). Replaces any currently visible toast. */
void ui_toast_show(ui_toast_type_t type, const char *message,
                   uint32_t duration_ms);

/* Dismiss the current toast immediately (if any). */
void ui_toast_dismiss(void);

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
