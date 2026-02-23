/*
 * sim_main.c — Lyra UI simulator entry point.
 *
 * Creates a 720×1280 SDL2 window using LVGL 9's built-in SDL driver,
 * initializes the UI, and runs the main loop.
 *
 * Build:
 *   cd simulator
 *   cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
 *   cmake --build build
 *   ./build/lyra_sim          (or build\Debug\lyra_sim.exe on Windows)
 */

#include "lvgl.h"
#include "ui.h"
#include <stdio.h>

/* SDL is pulled in by LVGL's SDL driver; we just need the header for SDL_Delay */
#include <SDL2/SDL.h>

/* Display dimensions (portrait, like the real device) */
#define SIM_HOR_RES  720
#define SIM_VER_RES  1280

/* UI refresh interval (ms) — how often ui_update() is called */
#define UI_UPDATE_INTERVAL_MS  200

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("Lyra UI Simulator — %dx%d portrait\n", SIM_HOR_RES, SIM_VER_RES);

    /* Initialize LVGL */
    lv_init();

    /* Create SDL display (LVGL 9 built-in SDL driver) */
    lv_display_t *disp = lv_sdl_window_create(SIM_HOR_RES, SIM_VER_RES);
    if (!disp) {
        fprintf(stderr, "Failed to create SDL display\n");
        return 1;
    }
    lv_sdl_window_set_title(disp, "Lyra — Now Playing");
    lv_sdl_window_set_zoom(disp, 1);

    /* Create SDL mouse input (emulates touch) */
    lv_sdl_mouse_create();

    /* Initialize the Lyra UI */
    ui_init();

    printf("UI initialized. Running main loop...\n");

    /* Main loop */
    uint32_t last_update = lv_tick_get();

    while (1) {
        uint32_t sleep_ms = lv_timer_handler();

        /* Periodically refresh UI data */
        uint32_t now = lv_tick_get();
        if (now - last_update >= UI_UPDATE_INTERVAL_MS) {
            ui_update();
            last_update = now;
        }

        /* Sleep to reduce CPU usage */
        if (sleep_ms > 50) sleep_ms = 50;
        if (sleep_ms < 1)  sleep_ms = 1;
        SDL_Delay(sleep_ms);
    }

    return 0;
}
