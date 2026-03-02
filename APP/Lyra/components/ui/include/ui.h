#pragma once

#include <stdint.h>

/*
 * ui.h — Public API for the Lyra UI (LVGL-based).
 *
 * Portable: uses only LVGL API + ui_data.h abstraction.
 * Same code runs on ESP32-P4 (MIPI DSI) and PC simulator (SDL2).
 */

/* Screen identifiers */
typedef enum {
    UI_SCREEN_NOW_PLAYING,
    UI_SCREEN_BROWSER,
    UI_SCREEN_SETTINGS,
    UI_SCREEN_WIFI,
    UI_SCREEN_NET_AUDIO,
    UI_SCREEN_USB_DAC,
    UI_SCREEN_EQ,
    UI_SCREEN_QUEUE,
    UI_SCREEN_ABOUT,
    UI_SCREEN_QOBUZ,
    UI_SCREEN_SUBSONIC,
    UI_SCREEN_LOCK,       /* Special: not in normal navigation flow */
    UI_SCREEN_COUNT
} ui_screen_t;

/* Display dimensions (portrait) */
#define UI_HOR_RES  720
#define UI_VER_RES  1280

/*
 * Initialize all UI screens and the theme.
 * Must be called after lv_init() and display driver setup.
 */
void ui_init(void);

/*
 * Refresh UI with latest data from ui_data_get_*().
 * Call this periodically (e.g. every 100–200 ms) from the UI task.
 */
void ui_update(void);

/*
 * Navigate to a specific screen with animated transition.
 */
void ui_navigate_to(ui_screen_t screen);

/*
 * Navigate back to the previous screen (before the current one).
 * Falls back to Settings if there is no meaningful previous screen.
 */
void ui_navigate_back(void);

/*
 * Get the currently active screen.
 */
ui_screen_t ui_get_current_screen(void);

/*
 * Lock / unlock the screen.
 * Lock saves current screen and shows lock screen (fade in).
 * Unlock restores the previous screen (fade out).
 */
void ui_lock(void);
void ui_unlock(void);
bool ui_is_locked(void);
