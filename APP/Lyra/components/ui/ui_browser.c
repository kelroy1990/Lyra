/*
 * ui_browser.c — File Browser screen for Lyra (720x1280 portrait).
 *
 * Layout (top to bottom):
 *
 *   ┌──────────────────────────────────┐
 *   │  ◀  /Music/Pink Floyd           │  Header (48px): back + path
 *   ├──────────────────────────────────┤
 *   │  📁 Albums                       │
 *   │  ─────────────────────────────   │
 *   │  📁 Live                         │
 *   │  ─────────────────────────────   │
 *   │  🎵 01 - Shine On You.flac      │
 *   │  ─────────────────────────────   │
 *   │  🎵 02 - Welcome to the...      │
 *   │          ... (scrollable) ...    │
 *   ├──────────────────────────────────┤
 *   │  🎵 Playing   📁 Files   ⚙ Set  │  Nav bar (56px)
 *   └──────────────────────────────────┘
 */

#include "ui_internal.h"
#include <string.h>

/* Layout constants */
#define PAD_SIDE  24

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_browser;

/* Header */
static lv_obj_t *btn_back;
static lv_obj_t *lbl_path;

/* Scrollable list */
static lv_obj_t *list_container;

/* Track last known path to avoid unnecessary rebuilds */
static char s_last_path[256];
static int  s_last_count = -1;

/* -----------------------------------------------------------------------
 * Entry click user data — we store index + type in a small static array
 * so we can resolve which entry was tapped.
 * ----------------------------------------------------------------------- */

typedef struct {
    char name[128];
    ui_entry_type_t type;
} entry_ctx_t;

static entry_ctx_t s_entry_ctx[UI_BROWSER_MAX_ITEMS];

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_cmd_browse_back();
    /* Force rebuild on next update */
    s_last_count = -1;
}

static void on_entry_click(lv_event_t *e)
{
    entry_ctx_t *ctx = (entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    if (ctx->type == UI_ENTRY_FOLDER) {
        ui_cmd_browse_open(ctx->name);
        s_last_count = -1;  /* Force rebuild */
    } else {
        ui_cmd_browse_play(ctx->name);
        /* Navigate to Now Playing after selecting a file */
        ui_navigate_to(UI_SCREEN_NOW_PLAYING);
    }
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_browser_create(void)
{
    scr_browser = lv_obj_create(NULL);
    lv_obj_add_style(scr_browser, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_browser, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main = lv_obj_create(scr_browser);
    lv_obj_remove_style_all(main);
    lv_obj_set_size(main, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_pad_gap(main, 0, 0);
    lv_obj_set_scrollbar_mode(main, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * HEADER BAR (48px)
     * ================================================================== */
    lv_obj_t *header = lv_obj_create(main);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, ui_theme_style_status_bar(), 0);
    lv_obj_set_size(header, UI_HOR_RES, 48);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 8, 0);

    /* Back button */
    btn_back = lv_button_create(header);
    lv_obj_add_style(btn_back, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_back, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn_back, 8, 0);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);

    /* Path label */
    lbl_path = lv_label_create(header);
    lv_obj_add_style(lbl_path, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_path, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_path, "/");
    lv_label_set_long_mode(lbl_path, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(lbl_path, 1);

    /* ==================================================================
     * FILE LIST (scrollable, fills remaining space)
     * ================================================================== */
    list_container = lv_obj_create(main);
    lv_obj_remove_style_all(list_container);
    lv_obj_set_width(list_container, UI_HOR_RES);
    lv_obj_set_flex_grow(list_container, 1);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 0, 0);
    lv_obj_set_scrollbar_mode(list_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(list_container, LV_OPA_TRANSP, 0);

    /* ==================================================================
     * BOTTOM NAVIGATION BAR (56px)
     * ================================================================== */
    ui_create_nav_bar(main, UI_SCREEN_BROWSER);

    /* Init tracking state */
    s_last_path[0] = '\0';
    s_last_count = -1;

    return scr_browser;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh browser data)
 * ----------------------------------------------------------------------- */

void ui_browser_update(const ui_browser_data_t *data)
{
    if (!scr_browser) return;

    /* -- Update path display -- */
    lv_label_set_text(lbl_path, data->path);

    /* -- Show/hide back button -- */
    if (data->at_root)
        lv_obj_add_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(btn_back, LV_OBJ_FLAG_HIDDEN);

    /* -- Skip list rebuild if nothing changed -- */
    if (s_last_count == data->item_count &&
        strcmp(s_last_path, data->path) == 0) {
        return;
    }

    /* Save current state for dirty-check */
    strncpy(s_last_path, data->path, sizeof(s_last_path) - 1);
    s_last_path[sizeof(s_last_path) - 1] = '\0';
    s_last_count = data->item_count;

    /* -- Rebuild list -- */
    lv_obj_clean(list_container);
    lv_obj_scroll_to_y(list_container, 0, LV_ANIM_OFF);

    for (int i = 0; i < data->item_count; i++) {
        const ui_browser_entry_t *entry = &data->items[i];

        /* Copy entry info to static context for event handler */
        strncpy(s_entry_ctx[i].name, entry->name, sizeof(s_entry_ctx[i].name) - 1);
        s_entry_ctx[i].name[sizeof(s_entry_ctx[i].name) - 1] = '\0';
        s_entry_ctx[i].type = entry->type;

        /* Row container (clickable) */
        lv_obj_t *row = lv_obj_create(list_container);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, ui_theme_style_list_item(), 0);
        lv_obj_set_size(row, UI_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 12, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

        /* Pressed feedback */
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        /* Icon */
        lv_obj_t *icon = lv_label_create(row);
        lv_obj_add_style(icon, ui_theme_style_info(), 0);
        if (entry->type == UI_ENTRY_FOLDER) {
            lv_label_set_text(icon, LV_SYMBOL_DIRECTORY);
            lv_obj_set_style_text_color(icon, ui_theme_color_accent(), 0);
        } else {
            lv_label_set_text(icon, LV_SYMBOL_AUDIO);
            lv_obj_set_style_text_color(icon,
                                        ui_theme_color_text_secondary(), 0);
        }

        /* Name label */
        lv_obj_t *name = lv_label_create(row);
        lv_obj_add_style(name, ui_theme_style_list_item(), 0);
        lv_label_set_text(name, entry->name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);

        /* Folder indicator (chevron) */
        if (entry->type == UI_ENTRY_FOLDER) {
            lv_obj_t *chevron = lv_label_create(row);
            lv_obj_add_style(chevron, ui_theme_style_info(), 0);
            lv_obj_set_style_text_color(chevron,
                                        ui_theme_color_text_secondary(), 0);
            lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        }

        /* Click event */
        lv_obj_add_event_cb(row, on_entry_click, LV_EVENT_CLICKED,
                            &s_entry_ctx[i]);

        /* Divider line (skip after last item) */
        if (i < data->item_count - 1) {
            lv_obj_t *div = lv_obj_create(list_container);
            lv_obj_remove_style_all(div);
            lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
            lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
            lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
        }
    }

    /* Empty state */
    if (data->item_count == 0) {
        lv_obj_t *empty = lv_label_create(list_container);
        lv_obj_add_style(empty, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(empty,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(empty, "Empty folder");
        lv_obj_set_width(empty, UI_HOR_RES);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(empty, 40, 0);
    }
}
