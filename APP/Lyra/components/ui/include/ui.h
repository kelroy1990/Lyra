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
 * Get the currently active screen.
 */
ui_screen_t ui_get_current_screen(void);
