/*
 * ui_init.c — Screen manager and navigation for Lyra UI.
 */

#include "ui_internal.h"
#include <stdint.h>
#include <stdbool.h>

static lv_obj_t   *s_screens[UI_SCREEN_COUNT];
static ui_screen_t  s_current = UI_SCREEN_NOW_PLAYING;
static ui_screen_t  s_prev    = UI_SCREEN_NOW_PLAYING;

/* Lock state machine */
static bool         s_locked          = false;
static ui_screen_t  s_pre_lock_screen = UI_SCREEN_NOW_PLAYING;
#define LOCK_TIMEOUT_MS  15000  /* 15 seconds for simulator testing */

/* -----------------------------------------------------------------------
 * Shared nav bar
 * ----------------------------------------------------------------------- */

static void on_nav_tap(lv_event_t *e)
{
    ui_screen_t target = (ui_screen_t)(uintptr_t)lv_event_get_user_data(e);
    ui_navigate_to(target);
}

static const struct {
    ui_screen_t screen;
    const char *label;
} s_nav_tabs[] = {
    { UI_SCREEN_NOW_PLAYING, LV_SYMBOL_AUDIO " Playing"     },
    { UI_SCREEN_BROWSER,     LV_SYMBOL_DIRECTORY " Files"    },
    { UI_SCREEN_SETTINGS,    LV_SYMBOL_SETTINGS " Settings"  },
};
#define NAV_TAB_COUNT  (sizeof(s_nav_tabs) / sizeof(s_nav_tabs[0]))

lv_obj_t *ui_create_nav_bar(lv_obj_t *parent, ui_screen_t active_screen)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, ui_theme_style_nav_bar(), 0);
    lv_obj_set_size(bar, UI_HOR_RES, 56);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    for (size_t i = 0; i < NAV_TAB_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_add_style(btn, ui_theme_style_nav_item(), 0);

        if (s_nav_tabs[i].screen == active_screen) {
            lv_obj_add_style(btn, ui_theme_style_nav_item_active(), 0);
        } else {
            lv_obj_add_event_cb(btn, on_nav_tap, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)s_nav_tabs[i].screen);
        }

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_nav_tabs[i].label);
    }

    return bar;
}

void ui_init(void)
{
    ui_theme_init();

    /* Create all screens */
    s_screens[UI_SCREEN_NOW_PLAYING] = ui_now_playing_create();
    s_screens[UI_SCREEN_BROWSER]     = ui_browser_create();
    s_screens[UI_SCREEN_SETTINGS]    = ui_settings_create();
    s_screens[UI_SCREEN_WIFI]        = ui_wifi_create();
    s_screens[UI_SCREEN_NET_AUDIO]   = ui_net_audio_create();
    s_screens[UI_SCREEN_USB_DAC]     = ui_usb_dac_create();
    s_screens[UI_SCREEN_EQ]          = ui_eq_create();
    s_screens[UI_SCREEN_QUEUE]       = ui_queue_create();
    s_screens[UI_SCREEN_ABOUT]       = ui_about_create();
    s_screens[UI_SCREEN_QOBUZ]       = ui_qobuz_create();
    s_screens[UI_SCREEN_SUBSONIC]    = ui_subsonic_create();
    s_screens[UI_SCREEN_LOCK]        = ui_lock_create();

    /* Load the default screen */
    if (s_screens[UI_SCREEN_NOW_PLAYING]) {
        lv_screen_load(s_screens[UI_SCREEN_NOW_PLAYING]);
        s_current = UI_SCREEN_NOW_PLAYING;
    }
}

void ui_update(void)
{
    /* Auto-lock after inactivity */
    if (!s_locked) {
        uint32_t idle = lv_display_get_inactive_time(NULL);
        if (idle > LOCK_TIMEOUT_MS) {
            ui_lock();
        }
    }

    /* If locked, only update lock screen */
    if (s_locked) {
        ui_now_playing_t   np  = ui_data_get_now_playing();
        ui_system_status_t sys = ui_data_get_system_status();
        ui_lock_update(&np, &sys);
        return;
    }

    ui_now_playing_t   np  = ui_data_get_now_playing();
    ui_system_status_t sys = ui_data_get_system_status();

    switch (s_current) {
        case UI_SCREEN_NOW_PLAYING:
            ui_now_playing_update(&np, &sys);
            break;
        case UI_SCREEN_BROWSER: {
            ui_browser_data_t br = ui_data_get_browser();
            ui_browser_update(&br);
            break;
        }
        case UI_SCREEN_SETTINGS:
            ui_settings_update(&sys);
            break;
        case UI_SCREEN_WIFI: {
            ui_wifi_scan_data_t wifi = ui_data_get_wifi_scan();
            ui_wifi_update(&wifi);
            break;
        }
        case UI_SCREEN_NET_AUDIO: {
            ui_net_audio_data_t na = ui_data_get_net_audio();
            ui_net_audio_update(&na);
            break;
        }
        case UI_SCREEN_USB_DAC: {
            ui_usb_dac_data_t dac = ui_data_get_usb_dac();
            ui_usb_dac_update(&dac);
            break;
        }
        case UI_SCREEN_EQ: {
            ui_eq_presets_data_t presets = ui_data_get_eq_presets();
            ui_eq_update(&sys, &presets);
            break;
        }
        case UI_SCREEN_QUEUE: {
            ui_queue_data_t queue = ui_data_get_queue();
            ui_queue_update(&queue, &np);
            break;
        }
        case UI_SCREEN_ABOUT: {
            ui_device_info_t info = ui_data_get_device_info();
            ui_about_update(&info);
            break;
        }
        case UI_SCREEN_QOBUZ: {
            ui_qobuz_data_t qobuz = ui_data_get_qobuz();
            ui_qobuz_update(&qobuz);
            break;
        }
        case UI_SCREEN_SUBSONIC: {
            ui_subsonic_data_t subsonic = ui_data_get_subsonic();
            ui_subsonic_update(&subsonic);
            break;
        }
        default:
            break;
    }
}

void ui_navigate_to(ui_screen_t screen)
{
    if (screen >= UI_SCREEN_COUNT) return;
    if (!s_screens[screen]) return;
    if (screen == s_current) return;

    /* Block navigation while locked (only unlock goes through ui_unlock()) */
    if (s_locked) return;

    /* Choose animation based on navigation direction:
     * - Main tabs (Playing/Browser/Settings): slide left/right by index order
     * - Sub-screens (WiFi, About, etc.): slide up to enter, down to leave */
    bool entering_sub = (screen == UI_SCREEN_WIFI || screen == UI_SCREEN_NET_AUDIO ||
                         screen == UI_SCREEN_USB_DAC || screen == UI_SCREEN_EQ ||
                         screen == UI_SCREEN_QUEUE || screen == UI_SCREEN_ABOUT ||
                         screen == UI_SCREEN_QOBUZ || screen == UI_SCREEN_SUBSONIC);
    bool leaving_sub  = (s_current == UI_SCREEN_WIFI || s_current == UI_SCREEN_NET_AUDIO ||
                         s_current == UI_SCREEN_USB_DAC || s_current == UI_SCREEN_EQ ||
                         s_current == UI_SCREEN_QUEUE || s_current == UI_SCREEN_ABOUT ||
                         s_current == UI_SCREEN_QOBUZ || s_current == UI_SCREEN_SUBSONIC);

    lv_scr_load_anim_t anim;
    if (entering_sub)
        anim = LV_SCR_LOAD_ANIM_MOVE_TOP;
    else if (leaving_sub)
        anim = LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
    else if (screen > s_current)
        anim = LV_SCR_LOAD_ANIM_MOVE_LEFT;
    else
        anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;

    lv_screen_load_anim(s_screens[screen], anim, 250, 0, false);
    s_prev    = s_current;
    s_current = screen;
}

void ui_navigate_back(void)
{
    ui_navigate_to(s_prev);
}

ui_screen_t ui_get_current_screen(void)
{
    return s_current;
}

/* -----------------------------------------------------------------------
 * Lock / Unlock
 * ----------------------------------------------------------------------- */

void ui_lock(void)
{
    if (s_locked) return;
    if (!s_screens[UI_SCREEN_LOCK]) return;

    s_pre_lock_screen = s_current;
    s_locked = true;
    lv_screen_load_anim(s_screens[UI_SCREEN_LOCK],
                        LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, false);
    s_current = UI_SCREEN_LOCK;
}

void ui_unlock(void)
{
    if (!s_locked) return;
    if (!s_screens[s_pre_lock_screen]) return;

    s_locked = false;
    lv_screen_load_anim(s_screens[s_pre_lock_screen],
                        LV_SCR_LOAD_ANIM_FADE_OUT, 300, 0, false);
    s_current = s_pre_lock_screen;
}

bool ui_is_locked(void)
{
    return s_locked;
}
