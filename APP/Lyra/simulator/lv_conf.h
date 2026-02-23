/*
 * lv_conf.h — LVGL configuration for the Lyra PC simulator.
 *
 * This configures LVGL 9.x for a 720×1280 SDL2 display on desktop.
 * The ESP-IDF build uses a different lv_conf.h managed by the lvgl component.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* -----------------------------------------------------------------------
 * Color settings
 * ----------------------------------------------------------------------- */

#define LV_COLOR_DEPTH          16      /* Match ESP target (RGB565) */

/* -----------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------- */

#define LV_MEM_CUSTOM           0
#define LV_MEM_SIZE             (2 * 1024 * 1024)   /* 2 MB for simulator */

/* -----------------------------------------------------------------------
 * Display
 * ----------------------------------------------------------------------- */

#define LV_DPI_DEF              200     /* Approximate for 5" 720×1280 */

/* -----------------------------------------------------------------------
 * SDL driver (built-in to LVGL 9)
 * ----------------------------------------------------------------------- */

#define LV_USE_SDL              1
#define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT        2
#define LV_SDL_FULLSCREEN       0
#define LV_SDL_DIRECT_EXIT      1

/* -----------------------------------------------------------------------
 * Fonts
 * ----------------------------------------------------------------------- */

#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/* -----------------------------------------------------------------------
 * Widgets
 * ----------------------------------------------------------------------- */

#define LV_USE_LABEL            1
#define LV_USE_BUTTON           1
#define LV_USE_SLIDER           1
#define LV_USE_BAR              1
#define LV_USE_IMAGE            1
#define LV_USE_LINE             1
#define LV_USE_ARC              1
#define LV_USE_TEXTAREA         0
#define LV_USE_DROPDOWN         0
#define LV_USE_ROLLER           0
#define LV_USE_TABLE            0
#define LV_USE_CHART            0
#define LV_USE_CALENDAR         0
#define LV_USE_KEYBOARD         0
#define LV_USE_LIST             1
#define LV_USE_MSGBOX           0
#define LV_USE_SPINBOX          0
#define LV_USE_SPINNER          0
#define LV_USE_TABVIEW          0
#define LV_USE_TILEVIEW         0
#define LV_USE_WIN              0
#define LV_USE_SPAN             0
#define LV_USE_SWITCH           1
#define LV_USE_CHECKBOX         0
#define LV_USE_LED              0
#define LV_USE_MENU             0
#define LV_USE_CANVAS           0

/* -----------------------------------------------------------------------
 * Layouts
 * ----------------------------------------------------------------------- */

#define LV_USE_FLEX             1
#define LV_USE_GRID             1

/* -----------------------------------------------------------------------
 * Animations
 * ----------------------------------------------------------------------- */

#define LV_USE_ANIMATION        1

/* -----------------------------------------------------------------------
 * Logging
 * ----------------------------------------------------------------------- */

#define LV_USE_LOG              1
#define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF           1

/* -----------------------------------------------------------------------
 * OS / Tick
 * ----------------------------------------------------------------------- */

#define LV_TICK_CUSTOM          0
#define LV_USE_OS               LV_OS_NONE

/* -----------------------------------------------------------------------
 * Stdlib
 * ----------------------------------------------------------------------- */

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/* -----------------------------------------------------------------------
 * Symbols (built-in icon font)
 * ----------------------------------------------------------------------- */

#define LV_USE_FONT_PLACEHOLDER 1

/* Disable unused features for faster compile */
#define LV_USE_SYSMON           0
#define LV_USE_PROFILER         0
#define LV_USE_OBSERVER         1
#define LV_USE_IME_PINYIN       0
#define LV_USE_FILE_EXPLORER    0
#define LV_USE_BARCODE          0
#define LV_USE_QRCODE           0
#define LV_USE_GIF              0
#define LV_USE_TINY_TTF         0
#define LV_USE_FREETYPE         0
#define LV_USE_LODEPNG          0
#define LV_USE_LIBPNG           0
#define LV_USE_BMP              0
#define LV_USE_SJPG             0
#define LV_USE_LIBJPEG_TURBO    0
#define LV_USE_RLE              0
#define LV_USE_FFMPEG           0

/* Disable other drivers */
#define LV_USE_LINUX_FBDEV      0
#define LV_USE_LINUX_DRM        0
#define LV_USE_NUTTX            0
#define LV_USE_WINDOWS          0
#define LV_USE_X11              0
#define LV_USE_WAYLAND          0
#define LV_USE_OPENGLES         0

#endif /* LV_CONF_H */
