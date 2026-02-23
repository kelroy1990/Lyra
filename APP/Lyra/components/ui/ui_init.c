/*
 * ui_init.c — Screen manager and navigation for Lyra UI.
 */

#include "ui_internal.h"
#include <stdint.h>

static lv_obj_t   *s_screens[UI_SCREEN_COUNT];
static ui_screen_t  s_current = UI_SCREEN_NOW_PLAYING;

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

    /* Load the default screen */
    if (s_screens[UI_SCREEN_NOW_PLAYING]) {
        lv_screen_load(s_screens[UI_SCREEN_NOW_PLAYING]);
        s_current = UI_SCREEN_NOW_PLAYING;
    }
}

void ui_update(void)
{
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
        default:
            break;
    }
}

void ui_navigate_to(ui_screen_t screen)
{
    if (screen >= UI_SCREEN_COUNT) return;
    if (!s_screens[screen]) return;
    if (screen == s_current) return;

    lv_screen_load_anim(s_screens[screen], LV_SCR_LOAD_ANIM_FADE_IN,
                        300, 0, false);
    s_current = screen;
}

ui_screen_t ui_get_current_screen(void)
{
    return s_current;
}
