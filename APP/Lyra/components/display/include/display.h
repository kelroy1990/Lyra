/**
 * @file    display.h
 * @date    2026-02-11
 * @brief   Public API for display and LVGL subsystem
 *
 * Will expose:
 * - display_init()           : Init MIPI DSI, framebuffer, LVGL, touch
 * - display_start()          : Launch LVGL task
 * - display_set_backlight()  : Brightness control
 * - display_lock() / unlock(): Thread-safe LVGL access from other tasks
 */
