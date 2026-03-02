/*
 * ui_subsonic.c — Subsonic / Navidrome sub-screen for Lyra.
 *
 * Supports five views within a single screen:
 *   - PROFILES:      Saved server list (first screen)
 *   - PROFILE_EDIT:  Add / edit server form
 *   - ALBUMS:        Recent album list
 *   - TRACKS:        Track list inside a selected album
 *   - SEARCH:        Search results (tracks)
 */

#include "ui_internal.h"
#include <string.h>
#include <stdio.h>

/* Layout constants */
#define PAD_SIDE  24
#define CONTENT_W (UI_HOR_RES - 2 * PAD_SIDE)

/* Auth dot colors */
#define COL_AUTH_OK       lv_color_hex(0x4CAF50)
#define COL_AUTH_LOGGING  lv_color_hex(0xFFA726)
#define COL_AUTH_FAILED   lv_color_hex(0xFF5252)
#define COL_AUTH_IDLE     lv_color_hex(0x555555)

/* -----------------------------------------------------------------------
 * Widget references
 * ----------------------------------------------------------------------- */

static lv_obj_t *scr_subsonic;
static lv_obj_t *content;        /* Scrollable area */

/* Header */
static lv_obj_t *dot_auth;

/* --- Profile list view --- */
static lv_obj_t *profile_list;

/* --- Profile edit view --- */
static lv_obj_t *form_container;
static lv_obj_t *ta_form_name;
static lv_obj_t *ta_form_url;
static lv_obj_t *ta_form_user;
static lv_obj_t *ta_form_pass;
static lv_obj_t *btn_form_save;
static lv_obj_t *btn_form_delete;
static lv_obj_t *kb_form;

/* --- Browse views (albums/tracks/search) --- */
static lv_obj_t *browse_container;   /* Holds search + section + item_list */
static lv_obj_t *ta_search;
static lv_obj_t *kb_search;
static lv_obj_t *lbl_search_header;
static lv_obj_t *lbl_section;
static lv_obj_t *div_section;
static lv_obj_t *item_list;

/* Dirty-check state */
static ui_subsonic_view_t s_last_view          = (ui_subsonic_view_t)-1;
static int                s_last_profile_count = -1;
static int                s_last_editing_idx   = -2;
static int                s_last_album_count   = -1;
static int                s_last_track_count   = -1;

/* Static ID storage for event user_data */
static char s_album_ids[UI_SUBSONIC_MAX_ITEMS][48];
static char s_track_ids[UI_SUBSONIC_MAX_ITEMS][48];
static char s_profile_ids[UI_SUBSONIC_MAX_PROFILES][16];
static int  s_profile_indices[UI_SUBSONIC_MAX_PROFILES];

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */

static void build_profile_list_content(const ui_subsonic_data_t *data);
static void build_profile_edit_content(const ui_subsonic_data_t *data);
static void build_album_list(const ui_subsonic_data_t *data);
static void build_track_list(const ui_subsonic_data_t *data);

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */

static void on_back(lv_event_t *e)
{
    (void)e;
    if (kb_search) lv_obj_add_flag(kb_search, LV_OBJ_FLAG_HIDDEN);
    if (kb_form)   lv_obj_add_flag(kb_form, LV_OBJ_FLAG_HIDDEN);

    if (s_last_view == UI_SUBSONIC_VIEW_PROFILES) {
        ui_navigate_back();
    } else {
        ui_cmd_subsonic_back();
    }
}

/* --- Profile events --- */

static void on_profile_click(lv_event_t *e)
{
    const char *pid = (const char *)lv_event_get_user_data(e);
    if (pid && pid[0])
        ui_cmd_subsonic_select_profile(pid);
}

static void on_profile_edit_btn(lv_event_t *e)
{
    int *idx = (int *)lv_event_get_user_data(e);
    if (idx)
        ui_cmd_subsonic_edit_profile(*idx);
}

static void on_add_profile(lv_event_t *e)
{
    (void)e;
    ui_cmd_subsonic_edit_profile(-1);
}

static void on_form_save(lv_event_t *e)
{
    (void)e;
    const char *name = lv_textarea_get_text(ta_form_name);
    const char *url  = lv_textarea_get_text(ta_form_url);
    const char *user = lv_textarea_get_text(ta_form_user);
    const char *pass = lv_textarea_get_text(ta_form_pass);

    if (!name || !name[0] || !url || !url[0]) return;

    if (kb_form) lv_obj_add_flag(kb_form, LV_OBJ_FLAG_HIDDEN);

    /* Generate or reuse ID */
    char id_buf[16];
    ui_subsonic_data_t snap = ui_data_get_subsonic();
    if (snap.editing_profile_idx >= 0 &&
        snap.editing_profile_idx < snap.profile_count) {
        strncpy(id_buf, snap.profiles[snap.editing_profile_idx].id,
                sizeof(id_buf) - 1);
        id_buf[sizeof(id_buf) - 1] = '\0';
    } else {
        lv_snprintf(id_buf, sizeof(id_buf), "p%d", snap.profile_count + 1);
    }

    ui_cmd_subsonic_save_profile(id_buf, name, url, user, pass);
}

static void on_form_delete(lv_event_t *e)
{
    (void)e;
    if (kb_form) lv_obj_add_flag(kb_form, LV_OBJ_FLAG_HIDDEN);

    ui_subsonic_data_t snap = ui_data_get_subsonic();
    if (snap.editing_profile_idx >= 0 &&
        snap.editing_profile_idx < snap.profile_count) {
        ui_cmd_subsonic_delete_profile(
            snap.profiles[snap.editing_profile_idx].id);
    }
}

/* --- Form keyboard events --- */

static void on_form_ta_focus(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (kb_form) {
        lv_keyboard_set_textarea(kb_form, ta);
        lv_obj_clear_flag(kb_form, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_form_ta_defocus(lv_event_t *e)
{
    (void)e;
    /* Don't hide immediately — user may be tapping another field */
}

static void on_form_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        lv_obj_add_flag(kb_form, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --- Album/track events --- */

static void on_album_click(lv_event_t *e)
{
    const char *album_id = (const char *)lv_event_get_user_data(e);
    if (album_id && album_id[0])
        ui_cmd_subsonic_open_album(album_id);
}

static void on_track_click(lv_event_t *e)
{
    const char *track_id = (const char *)lv_event_get_user_data(e);
    if (track_id && track_id[0]) {
        ui_cmd_subsonic_play_track(track_id);
        ui_navigate_to(UI_SCREEN_NOW_PLAYING);
    }
}

static void on_album_back(lv_event_t *e)
{
    (void)e;
    ui_cmd_subsonic_back();
}

static void on_search_submit(lv_event_t *e)
{
    (void)e;
    const char *query = lv_textarea_get_text(ta_search);
    if (query && query[0]) {
        ui_cmd_subsonic_search(query);
        if (kb_search) lv_obj_add_flag(kb_search, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb_search, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_READY) {
        on_search_submit(e);
    }
}

static void on_ta_focus(lv_event_t *e)
{
    (void)e;
    if (kb_search) {
        lv_keyboard_set_textarea(kb_search, ta_search);
        lv_obj_clear_flag(kb_search, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_ta_defocus(lv_event_t *e)
{
    (void)e;
    if (kb_search) lv_obj_add_flag(kb_search, LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static lv_obj_t *create_section(lv_obj_t *parent, const char *text,
                                int32_t margin_top)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_add_style(lbl, ui_theme_style_section_header(), 0);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, UI_HOR_RES);
    lv_obj_set_style_margin_top(lbl, margin_top, 0);
    return lbl;
}

static void fmt_duration(char *buf, size_t sz, uint32_t secs)
{
    uint32_t m = secs / 60;
    uint32_t s = secs % 60;
    lv_snprintf(buf, (uint32_t)sz, "%lu:%02lu",
                (unsigned long)m, (unsigned long)s);
}

static void add_divider(lv_obj_t *parent)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_add_style(div, ui_theme_style_list_divider(), 0);
    lv_obj_set_size(div, UI_HOR_RES - 2 * PAD_SIDE, 1);
    lv_obj_set_style_margin_left(div, PAD_SIDE, 0);
}

/* Create a labeled textarea for forms */
static lv_obj_t *create_form_field(lv_obj_t *parent, const char *label_text,
                                    const char *placeholder,
                                    const char *initial_value,
                                    bool is_password)
{
    /* Label */
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_add_style(lbl, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_margin_top(lbl, 12, 0);

    /* Textarea */
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(ta, 4, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, initial_value ? initial_value : "");
    if (is_password) lv_textarea_set_password_mode(ta, true);

    /* Style */
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_pad_all(ta, 12, 0);
    lv_obj_set_style_border_color(ta, ui_theme_color_accent(),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(ta, ui_theme_color_accent(), LV_PART_CURSOR);

    lv_obj_add_event_cb(ta, on_form_ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_form_ta_defocus, LV_EVENT_DEFOCUSED, NULL);

    return ta;
}

/* Style keyboard (shared between search and form) */
static void style_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);

    lv_obj_set_style_bg_color(kb, lv_color_hex(0x252525),
                              (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER,
                            (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(0x3A3A3A),
                                  (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(0xFFFFFF),
                                (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_16,
                               (uint32_t)LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x3A3A3A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1A1A1A),
                              (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, ui_theme_color_accent(),
                                (uint32_t)LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* -----------------------------------------------------------------------
 * Build profile list
 * ----------------------------------------------------------------------- */

static void build_profile_list_content(const ui_subsonic_data_t *data)
{
    lv_obj_clean(profile_list);

    /* Section header */
    create_section(profile_list, "MY SERVERS", 16);

    for (int i = 0; i < data->profile_count && i < UI_SUBSONIC_MAX_PROFILES; i++) {
        const ui_subsonic_profile_t *p = &data->profiles[i];

        /* Cache ID and index */
        strncpy(s_profile_ids[i], p->id, sizeof(s_profile_ids[i]) - 1);
        s_profile_ids[i][sizeof(s_profile_ids[i]) - 1] = '\0';
        s_profile_indices[i] = i;

        /* Card container */
        lv_obj_t *card = lv_obj_create(profile_list);
        lv_obj_remove_style_all(card);
        lv_obj_add_style(card, ui_theme_style_card(), 0);
        lv_obj_set_size(card, CONTENT_W, LV_SIZE_CONTENT);
        lv_obj_set_style_margin_top(card, 10, 0);
        lv_obj_set_style_margin_left(card, PAD_SIDE, 0);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        /* Pressed feedback */
        lv_obj_set_style_bg_color(card, lv_color_hex(0x252525), LV_STATE_PRESSED);

        /* Connected indicator: gold left border */
        if (p->connected) {
            lv_obj_set_style_border_width(card, 3, 0);
            lv_obj_set_style_border_color(card, ui_theme_color_accent(), 0);
            lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        }

        /* Left: text column (non-clickable so clicks reach the card) */
        lv_obj_t *text_col = lv_obj_create(card);
        lv_obj_remove_style_all(text_col);
        lv_obj_clear_flag(text_col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(text_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text_col, 1);
        lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(text_col, 4, 0);
        lv_obj_set_scrollbar_mode(text_col, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *lbl_name = lv_label_create(text_col);
        lv_obj_add_style(lbl_name, ui_theme_style_list_item(), 0);
        lv_label_set_text(lbl_name, p->name);
        lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_name, CONTENT_W - 80);

        char sub_buf[256];
        lv_snprintf(sub_buf, sizeof(sub_buf), "%s @ %s",
                    p->username, p->server_url);
        lv_obj_t *lbl_sub = lv_label_create(text_col);
        lv_obj_add_style(lbl_sub, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(lbl_sub,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(lbl_sub, sub_buf);
        lv_label_set_long_mode(lbl_sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_sub, CONTENT_W - 80);

        /* Right: edit button */
        lv_obj_t *btn_edit = lv_button_create(card);
        lv_obj_add_style(btn_edit, ui_theme_style_btn(), 0);
        lv_obj_add_style(btn_edit, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(btn_edit, 10, 0);
        lv_obj_t *lbl_edit = lv_label_create(btn_edit);
        lv_label_set_text(lbl_edit, LV_SYMBOL_SETTINGS);
        lv_obj_add_event_cb(btn_edit, on_profile_edit_btn, LV_EVENT_CLICKED,
                            &s_profile_indices[i]);

        /* Click on card = select profile */
        lv_obj_add_event_cb(card, on_profile_click, LV_EVENT_CLICKED,
                            (void *)s_profile_ids[i]);
    }

    /* "+ Add New Server" button */
    lv_obj_t *add_row = lv_obj_create(profile_list);
    lv_obj_remove_style_all(add_row);
    lv_obj_set_size(add_row, UI_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(add_row, 20, 0);
    lv_obj_set_flex_flow(add_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(add_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(add_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *btn_add = lv_button_create(add_row);
    lv_obj_set_style_bg_opa(btn_add, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn_add, ui_theme_color_accent(), 0);
    lv_obj_set_style_border_width(btn_add, 2, 0);
    lv_obj_set_style_border_opa(btn_add, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_add, 24, 0);
    lv_obj_set_style_pad_hor(btn_add, 28, 0);
    lv_obj_set_style_pad_ver(btn_add, 12, 0);
    lv_obj_set_style_shadow_width(btn_add, 0, 0);
    lv_obj_set_style_bg_color(btn_add, ui_theme_color_accent(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn_add, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *lbl_add = lv_label_create(btn_add);
    lv_obj_set_style_text_color(lbl_add, ui_theme_color_accent(), 0);
    lv_obj_set_style_text_color(lbl_add, lv_color_hex(0x0D0D0D), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(lbl_add, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_add, LV_SYMBOL_PLUS "  Add New Server");
    lv_obj_add_event_cb(btn_add, on_add_profile, LV_EVENT_CLICKED, NULL);
}

/* -----------------------------------------------------------------------
 * Build profile edit form
 * ----------------------------------------------------------------------- */

static void build_profile_edit_content(const ui_subsonic_data_t *data)
{
    lv_obj_clean(form_container);

    bool is_new = (data->editing_profile_idx < 0 ||
                   data->editing_profile_idx >= data->profile_count);

    const char *init_name = "";
    const char *init_url  = "";
    const char *init_user = "";

    if (!is_new) {
        const ui_subsonic_profile_t *p =
            &data->profiles[data->editing_profile_idx];
        init_name = p->name;
        init_url  = p->server_url;
        init_user = p->username;
    }

    /* Title */
    lv_obj_t *lbl_title = lv_label_create(form_container);
    lv_obj_add_style(lbl_title, ui_theme_style_section_header(), 0);
    lv_label_set_text(lbl_title, is_new ? "NEW SERVER" : "EDIT SERVER");
    lv_obj_set_style_margin_top(lbl_title, 16, 0);

    /* Form fields */
    ta_form_name = create_form_field(form_container, "Name", "My Server",
                                      init_name, false);
    ta_form_url  = create_form_field(form_container, "Server URL",
                                      "http://192.168.1.100:4533",
                                      init_url, false);
    ta_form_user = create_form_field(form_container, "Username", "admin",
                                      init_user, false);
    ta_form_pass = create_form_field(form_container, "Password", "password",
                                      "", true);

    /* "Connect & Save" button */
    btn_form_save = lv_button_create(form_container);
    lv_obj_set_size(btn_form_save, CONTENT_W, 48);
    lv_obj_set_style_margin_top(btn_form_save, 20, 0);
    lv_obj_set_style_bg_color(btn_form_save, ui_theme_color_accent(), 0);
    lv_obj_set_style_bg_opa(btn_form_save, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_form_save, 24, 0);
    lv_obj_set_style_shadow_width(btn_form_save, 0, 0);
    lv_obj_set_style_bg_color(btn_form_save, ui_theme_color_accent_dim(),
                              LV_STATE_PRESSED);

    lv_obj_t *lbl_save = lv_label_create(btn_form_save);
    lv_obj_set_style_text_color(lbl_save, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_save, "Connect & Save");
    lv_obj_center(lbl_save);
    lv_obj_add_event_cb(btn_form_save, on_form_save, LV_EVENT_CLICKED, NULL);

    /* "Delete" button (only for edit, not new) */
    if (!is_new) {
        btn_form_delete = lv_button_create(form_container);
        lv_obj_set_size(btn_form_delete, CONTENT_W, 44);
        lv_obj_set_style_margin_top(btn_form_delete, 12, 0);
        lv_obj_set_style_bg_opa(btn_form_delete, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(btn_form_delete, lv_color_hex(0xFF5252), 0);
        lv_obj_set_style_border_width(btn_form_delete, 2, 0);
        lv_obj_set_style_border_opa(btn_form_delete, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn_form_delete, 24, 0);
        lv_obj_set_style_shadow_width(btn_form_delete, 0, 0);
        lv_obj_set_style_bg_color(btn_form_delete, lv_color_hex(0xFF5252),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn_form_delete, LV_OPA_COVER,
                                LV_STATE_PRESSED);

        lv_obj_t *lbl_del = lv_label_create(btn_form_delete);
        lv_obj_set_style_text_color(lbl_del, lv_color_hex(0xFF5252), 0);
        lv_obj_set_style_text_color(lbl_del, lv_color_hex(0xFFFFFF),
                                    LV_STATE_PRESSED);
        lv_obj_set_style_text_font(lbl_del, &lv_font_montserrat_16, 0);
        lv_label_set_text(lbl_del, "Delete");
        lv_obj_center(lbl_del);
        lv_obj_add_event_cb(btn_form_delete, on_form_delete,
                            LV_EVENT_CLICKED, NULL);
    } else {
        btn_form_delete = NULL;
    }
}

/* -----------------------------------------------------------------------
 * Build album list
 * ----------------------------------------------------------------------- */

static void build_album_list(const ui_subsonic_data_t *data)
{
    lv_obj_clean(item_list);

    for (int i = 0; i < data->album_count && i < UI_SUBSONIC_MAX_ITEMS; i++) {
        const ui_subsonic_album_t *album = &data->albums[i];

        strncpy(s_album_ids[i], album->id, sizeof(s_album_ids[i]) - 1);
        s_album_ids[i][sizeof(s_album_ids[i]) - 1] = '\0';

        lv_obj_t *row = lv_obj_create(item_list);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, ui_theme_style_list_item(), 0);
        lv_obj_set_size(row, UI_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        lv_obj_t *lbl_name = lv_label_create(row);
        lv_obj_add_style(lbl_name, ui_theme_style_list_item(), 0);
        lv_label_set_text(lbl_name, album->name);
        lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_name, CONTENT_W - 20);

        lv_obj_t *sec_row = lv_obj_create(row);
        lv_obj_remove_style_all(sec_row);
        lv_obj_set_size(sec_row, CONTENT_W - 20, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sec_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sec_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(sec_row, LV_SCROLLBAR_MODE_OFF);

        char info_buf[192];
        lv_snprintf(info_buf, sizeof(info_buf), "%s \xc2\xb7 %d",
                    album->artist, album->year);

        lv_obj_t *lbl_info = lv_label_create(sec_row);
        lv_obj_add_style(lbl_info, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(lbl_info,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(lbl_info, info_buf);
        lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(lbl_info, 1);

        char count_buf[16];
        lv_snprintf(count_buf, sizeof(count_buf), "%d tracks",
                    album->song_count);

        lv_obj_t *lbl_count = lv_label_create(sec_row);
        lv_obj_add_style(lbl_count, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(lbl_count,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(lbl_count, count_buf);

        lv_obj_add_event_cb(row, on_album_click, LV_EVENT_CLICKED,
                            (void *)s_album_ids[i]);

        if (i < data->album_count - 1) add_divider(item_list);
    }
}

/* -----------------------------------------------------------------------
 * Build track list (for album view or search view)
 * ----------------------------------------------------------------------- */

static void build_track_list(const ui_subsonic_data_t *data)
{
    lv_obj_clean(item_list);

    /* Album back header (only in TRACKS view) */
    if (data->view == UI_SUBSONIC_VIEW_TRACKS &&
        data->current_album_name[0]) {
        lv_obj_t *back_row = lv_obj_create(item_list);
        lv_obj_remove_style_all(back_row);
        lv_obj_add_style(back_row, ui_theme_style_list_item(), 0);
        lv_obj_set_size(back_row, UI_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(back_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(back_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(back_row, 8, 0);
        lv_obj_add_flag(back_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(back_row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(back_row, lv_color_hex(0x1A1A1A),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back_row, LV_OPA_COVER, LV_STATE_PRESSED);

        lv_obj_t *lbl_arrow = lv_label_create(back_row);
        lv_obj_add_style(lbl_arrow, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(lbl_arrow, ui_theme_color_accent(), 0);
        lv_label_set_text(lbl_arrow, LV_SYMBOL_LEFT);

        lv_obj_t *lbl_album_name = lv_label_create(back_row);
        lv_obj_add_style(lbl_album_name, ui_theme_style_section_header(), 0);
        lv_label_set_text(lbl_album_name, data->current_album_name);
        lv_label_set_long_mode(lbl_album_name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(lbl_album_name, 1);

        lv_obj_add_event_cb(back_row, on_album_back, LV_EVENT_CLICKED, NULL);
        add_divider(item_list);
    }

    for (int i = 0; i < data->track_count && i < UI_SUBSONIC_MAX_ITEMS; i++) {
        const ui_subsonic_track_t *track = &data->tracks[i];

        strncpy(s_track_ids[i], track->id, sizeof(s_track_ids[i]) - 1);
        s_track_ids[i][sizeof(s_track_ids[i]) - 1] = '\0';

        lv_obj_t *row = lv_obj_create(item_list);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, ui_theme_style_list_item(), 0);
        lv_obj_set_size(row, UI_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        lv_obj_t *lbl_title = lv_label_create(row);
        lv_obj_add_style(lbl_title, ui_theme_style_list_item(), 0);
        lv_label_set_text(lbl_title, track->title);
        lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_title, CONTENT_W - 20);

        lv_obj_t *sec_row = lv_obj_create(row);
        lv_obj_remove_style_all(sec_row);
        lv_obj_set_size(sec_row, CONTENT_W - 20, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sec_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sec_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(sec_row, LV_SCROLLBAR_MODE_OFF);

        char info_buf[192];
        char dur_buf[16];
        fmt_duration(dur_buf, sizeof(dur_buf), track->duration_s);
        lv_snprintf(info_buf, sizeof(info_buf), "%s \xc2\xb7 %s",
                    track->artist, dur_buf);

        lv_obj_t *lbl_info = lv_label_create(sec_row);
        lv_obj_add_style(lbl_info, ui_theme_style_info(), 0);
        lv_obj_set_style_text_color(lbl_info,
                                    ui_theme_color_text_secondary(), 0);
        lv_label_set_text(lbl_info, info_buf);
        lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(lbl_info, 1);

        char fmt_buf[16];
        uint32_t sr_k = track->sample_rate / 1000;
        lv_snprintf(fmt_buf, sizeof(fmt_buf), "%d/%lu",
                    track->bits_per_sample, (unsigned long)sr_k);

        lv_obj_t *badge = lv_label_create(sec_row);
        if (track->is_hires) {
            lv_obj_add_style(badge, ui_theme_style_format_hires(), 0);
        } else {
            lv_obj_add_style(badge, ui_theme_style_info(), 0);
            lv_obj_set_style_text_color(badge,
                                        ui_theme_color_text_secondary(), 0);
        }
        lv_label_set_text(badge, fmt_buf);

        lv_obj_add_event_cb(row, on_track_click, LV_EVENT_CLICKED,
                            (void *)s_track_ids[i]);

        if (i < data->track_count - 1) add_divider(item_list);
    }
}

/* -----------------------------------------------------------------------
 * Screen creation
 * ----------------------------------------------------------------------- */

lv_obj_t *ui_subsonic_create(void)
{
    scr_subsonic = lv_obj_create(NULL);
    lv_obj_add_style(scr_subsonic, ui_theme_style_screen(), 0);
    lv_obj_set_scrollbar_mode(scr_subsonic, LV_SCROLLBAR_MODE_OFF);

    /* ---- Main flex-column container ---- */
    lv_obj_t *main_cont = lv_obj_create(scr_subsonic);
    lv_obj_remove_style_all(main_cont);
    lv_obj_set_size(main_cont, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_flex_flow(main_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main_cont, 0, 0);
    lv_obj_set_style_pad_gap(main_cont, 0, 0);
    lv_obj_set_scrollbar_mode(main_cont, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * HEADER BAR (48px)
     * ================================================================== */
    lv_obj_t *header = lv_obj_create(main_cont);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, ui_theme_style_status_bar(), 0);
    lv_obj_set_size(header, UI_HOR_RES, 48);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 10, 0);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_add_style(btn_back, ui_theme_style_btn(), 0);
    lv_obj_add_style(btn_back, ui_theme_style_btn_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn_back, 8, 0);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_hdr = lv_label_create(header);
    lv_obj_add_style(lbl_hdr, ui_theme_style_info(), 0);
    lv_obj_set_style_text_color(lbl_hdr, ui_theme_color_text_secondary(), 0);
    lv_label_set_text(lbl_hdr, "Subsonic");
    lv_obj_set_flex_grow(lbl_hdr, 1);

    dot_auth = lv_obj_create(header);
    lv_obj_remove_style_all(dot_auth);
    lv_obj_set_size(dot_auth, 12, 12);
    lv_obj_set_style_radius(dot_auth, 6, 0);
    lv_obj_set_style_bg_color(dot_auth, COL_AUTH_IDLE, 0);
    lv_obj_set_style_bg_opa(dot_auth, LV_OPA_COVER, 0);

    /* ==================================================================
     * SCROLLABLE CONTENT
     * ================================================================== */
    content = lv_obj_create(main_cont);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, UI_HOR_RES);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, 0, 0);
    lv_obj_set_style_pad_bottom(content, 16, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

    lv_obj_set_style_bg_color(content, lv_color_hex(0x333333), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, (lv_opa_t)120, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(content, 2, LV_PART_SCROLLBAR);

    /* ==================================================================
     * VIEW 1: Profile list container
     * ================================================================== */
    profile_list = lv_obj_create(content);
    lv_obj_remove_style_all(profile_list);
    lv_obj_set_width(profile_list, UI_HOR_RES);
    lv_obj_set_height(profile_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(profile_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(profile_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(profile_list, 0, 0);
    lv_obj_set_style_pad_bottom(profile_list, 20, 0);
    lv_obj_set_scrollbar_mode(profile_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(profile_list, LV_OBJ_FLAG_HIDDEN);

    /* ==================================================================
     * VIEW 2: Profile edit form container
     * ================================================================== */
    form_container = lv_obj_create(content);
    lv_obj_remove_style_all(form_container);
    lv_obj_set_width(form_container, UI_HOR_RES);
    lv_obj_set_height(form_container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(form_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(form_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(form_container, 0, 0);
    lv_obj_set_style_pad_bottom(form_container, 20, 0);
    lv_obj_set_scrollbar_mode(form_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(form_container, LV_OBJ_FLAG_HIDDEN);

    /* ==================================================================
     * VIEW 3-5: Browse container (search + section + item_list)
     * ================================================================== */
    browse_container = lv_obj_create(content);
    lv_obj_remove_style_all(browse_container);
    lv_obj_set_width(browse_container, UI_HOR_RES);
    lv_obj_set_height(browse_container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(browse_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(browse_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(browse_container, 0, 0);
    lv_obj_set_scrollbar_mode(browse_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(browse_container, LV_OBJ_FLAG_HIDDEN);

    /* Search section */
    lbl_search_header = create_section(browse_container, "SEARCH", 16);

    ta_search = lv_textarea_create(browse_container);
    lv_obj_set_size(ta_search, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(ta_search, 8, 0);
    lv_textarea_set_one_line(ta_search, true);
    lv_textarea_set_placeholder_text(ta_search,
                                     "Search albums, artists, tracks...");
    lv_textarea_set_text(ta_search, "");

    lv_obj_set_style_bg_color(ta_search, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(ta_search, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta_search, ui_theme_color_text_primary(), 0);
    lv_obj_set_style_text_font(ta_search, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(ta_search, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(ta_search, 1, 0);
    lv_obj_set_style_radius(ta_search, 8, 0);
    lv_obj_set_style_pad_all(ta_search, 12, 0);
    lv_obj_set_style_border_color(ta_search, ui_theme_color_accent(),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta_search, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(ta_search, ui_theme_color_accent(),
                              LV_PART_CURSOR);

    lv_obj_add_event_cb(ta_search, on_ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta_search, on_ta_defocus, LV_EVENT_DEFOCUSED, NULL);

    /* Dynamic section label + divider + item list */
    lbl_section = create_section(browse_container, "RECENT ALBUMS", 20);

    div_section = lv_obj_create(browse_container);
    lv_obj_remove_style_all(div_section);
    lv_obj_add_style(div_section, ui_theme_style_list_divider(), 0);
    lv_obj_set_size(div_section, CONTENT_W, 1);
    lv_obj_set_style_margin_left(div_section, PAD_SIDE, 0);

    item_list = lv_obj_create(browse_container);
    lv_obj_remove_style_all(item_list);
    lv_obj_set_width(item_list, UI_HOR_RES);
    lv_obj_set_height(item_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(item_list, 0, 0);
    lv_obj_set_scrollbar_mode(item_list, LV_SCROLLBAR_MODE_OFF);

    /* ==================================================================
     * KEYBOARDS (hidden by default)
     * ================================================================== */

    /* Search keyboard */
    kb_search = lv_keyboard_create(main_cont);
    lv_keyboard_set_textarea(kb_search, ta_search);
    lv_keyboard_set_mode(kb_search, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(kb_search, UI_HOR_RES, UI_VER_RES / 3);
    lv_obj_add_flag(kb_search, LV_OBJ_FLAG_HIDDEN);
    style_keyboard(kb_search);
    lv_obj_add_event_cb(kb_search, on_kb_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(kb_search, on_kb_event, LV_EVENT_READY, NULL);

    /* Form keyboard */
    kb_form = lv_keyboard_create(main_cont);
    lv_keyboard_set_mode(kb_form, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(kb_form, UI_HOR_RES, UI_VER_RES / 3);
    lv_obj_add_flag(kb_form, LV_OBJ_FLAG_HIDDEN);
    style_keyboard(kb_form);
    lv_obj_add_event_cb(kb_form, on_form_kb_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(kb_form, on_form_kb_event, LV_EVENT_READY, NULL);

    /* Init tracking */
    s_last_view          = (ui_subsonic_view_t)-1;
    s_last_profile_count = -1;
    s_last_editing_idx   = -2;
    s_last_album_count   = -1;
    s_last_track_count   = -1;

    return scr_subsonic;
}

/* -----------------------------------------------------------------------
 * Update (called periodically with fresh data)
 * ----------------------------------------------------------------------- */

void ui_subsonic_update(const ui_subsonic_data_t *data)
{
    if (!scr_subsonic) return;

    /* -- Auth dot color -- */
    lv_color_t auth_col;
    switch (data->auth_state) {
        case UI_SUBSONIC_AUTH_OK:         auth_col = COL_AUTH_OK;      break;
        case UI_SUBSONIC_AUTH_CONNECTING: auth_col = COL_AUTH_LOGGING; break;
        case UI_SUBSONIC_AUTH_FAILED:     auth_col = COL_AUTH_FAILED;  break;
        default:                          auth_col = COL_AUTH_IDLE;    break;
    }
    lv_obj_set_style_bg_color(dot_auth, auth_col, 0);

    /* -- Check if we need to rebuild -- */
    bool need_rebuild = false;

    if (data->view != s_last_view) {
        need_rebuild = true;
    } else if (data->view == UI_SUBSONIC_VIEW_PROFILES &&
               data->profile_count != s_last_profile_count) {
        need_rebuild = true;
    } else if (data->view == UI_SUBSONIC_VIEW_PROFILE_EDIT &&
               data->editing_profile_idx != s_last_editing_idx) {
        need_rebuild = true;
    } else if (data->view == UI_SUBSONIC_VIEW_ALBUMS &&
               data->album_count != s_last_album_count) {
        need_rebuild = true;
    } else if ((data->view == UI_SUBSONIC_VIEW_TRACKS ||
                data->view == UI_SUBSONIC_VIEW_SEARCH) &&
               data->track_count != s_last_track_count) {
        need_rebuild = true;
    }

    if (!need_rebuild) return;

    /* Hide all view containers */
    lv_obj_add_flag(profile_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(form_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(browse_container, LV_OBJ_FLAG_HIDDEN);

    switch (data->view) {
        case UI_SUBSONIC_VIEW_PROFILES:
            lv_obj_clear_flag(profile_list, LV_OBJ_FLAG_HIDDEN);
            build_profile_list_content(data);
            s_last_profile_count = data->profile_count;
            break;

        case UI_SUBSONIC_VIEW_PROFILE_EDIT:
            lv_obj_clear_flag(form_container, LV_OBJ_FLAG_HIDDEN);
            build_profile_edit_content(data);
            s_last_editing_idx = data->editing_profile_idx;
            break;

        case UI_SUBSONIC_VIEW_ALBUMS:
            lv_obj_clear_flag(browse_container, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_section, "RECENT ALBUMS");
            build_album_list(data);
            s_last_album_count = data->album_count;
            break;

        case UI_SUBSONIC_VIEW_TRACKS:
            lv_obj_clear_flag(browse_container, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_section, "TRACKS");
            build_track_list(data);
            s_last_track_count = data->track_count;
            break;

        case UI_SUBSONIC_VIEW_SEARCH:
            lv_obj_clear_flag(browse_container, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_section, "SEARCH RESULTS");
            build_track_list(data);
            s_last_track_count = data->track_count;
            break;
    }

    s_last_view = data->view;

    /* Scroll to top on view change */
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}
